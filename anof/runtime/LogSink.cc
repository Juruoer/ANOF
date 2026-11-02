#include <anof/runtime/LogSink.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
namespace anof::runtime {
namespace {
const char* name(LogEvent e) {
    switch (e) {
    case LogEvent::Access:
        return "http_access";
    case LogEvent::Ready:
        return "ANOF_READY";
    case LogEvent::Stopping:
        return "ANOF_STOPPING";
    case LogEvent::Stopped:
        return "ANOF_STOPPED";
    case LogEvent::Failure:
        return "failure";
    case LogEvent::Library:
        return "library_diagnostic_redacted";
    case LogEvent::Dropped:
        return "diagnostics_dropped";
    case LogEvent::AuditRetention:
        return "audit_retention";
    }
    return "failure";
}
std::string utc() {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}
bool token(std::string_view s, bool route = false) {
    if (s.size() > 160) return false;
    for (auto c : s)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_' && !(route && (c == '/' || c == ':' || c == '-'))) return false;
    return true;
}
}  // namespace
struct LogSink::Impl {
    struct Item {
        std::array<char, 4096> bytes{};
        std::size_t size = 0;
    };
    struct Cell {
        std::atomic<std::size_t> sequence{ 0 };
        Item item;
    };
    std::array<Cell, 1024> queue;
    std::atomic<std::size_t> enqueue{ 0 };
    std::size_t dequeue = 0;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<std::uint64_t> accepted{ 0 }, dropped{ 0 }, written{ 0 }, failures{ 0 };
    std::atomic<bool> stopping{ false };
    std::chrono::steady_clock::time_point deadline;
    std::thread worker;
    int out, err, outFlags, errFlags;
    Impl(int o, int e) : out(o), err(e), outFlags(::fcntl(o, F_GETFL)), errFlags(::fcntl(e, F_GETFL)) {
        if (outFlags < 0 || errFlags < 0 || ::fcntl(out, F_SETFL, outFlags | O_NONBLOCK) < 0) throw std::runtime_error("log descriptors unavailable");
        if (::fcntl(err, F_SETFL, errFlags | O_NONBLOCK) < 0) {
            ::fcntl(out, F_SETFL, outFlags);
            throw std::runtime_error("log descriptors unavailable");
        }
        for (std::size_t i = 0; i < queue.size(); ++i) queue[i].sequence.store(i);
        try {
            worker = std::thread([this] { consume(); });
        } catch (...) {
            ::fcntl(out, F_SETFL, outFlags);
            ::fcntl(err, F_SETFL, errFlags);
            throw;
        }
    }
    ~Impl() {
        stop();
        ::fcntl(out, F_SETFL, outFlags);
        ::fcntl(err, F_SETFL, errFlags);
    }
    bool expired() {
        std::lock_guard<std::mutex> lock(mutex);
        return stopping && std::chrono::steady_clock::now() >= deadline;
    }
    bool write(std::string_view bytes) {
        while (!bytes.empty()) {
            if (expired()) return false;
            auto n = ::write(out, bytes.data(), bytes.size());
            if (n > 0) {
                bytes.remove_prefix(static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                pollfd p{ out, POLLOUT, 0 };
                ::poll(&p, 1, 25);
                continue;
            }
            return false;
        }
        return true;
    }
    void fallback(std::uint64_t lost) {
        char b[160];
        auto n = std::snprintf(b, sizeof b, "{\"event\":\"log_write_failed\",\"dropped\":\"%llu\"}\n", static_cast<unsigned long long>(lost));
        if (n > 0) {
            auto ignored = ::write(err, b, static_cast<std::size_t>(n));
            (void)ignored;
        }
    }
    bool pop(Item& outItem) {
        auto& cell = queue[dequeue % queue.size()];
        if (cell.sequence.load(std::memory_order_acquire) != dequeue + 1) return false;
        outItem = cell.item;
        cell.sequence.store(dequeue + queue.size(), std::memory_order_release);
        ++dequeue;
        return true;
    }
    bool push(std::string_view line) {
        if (stopping.load()) return false;
        auto pos = enqueue.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            auto& cell = queue[pos % queue.size()];
            auto seq = cell.sequence.load(std::memory_order_acquire);
            if (seq < pos) return false;
            if (seq != pos) {
                pos = enqueue.load(std::memory_order_relaxed);
                continue;
            }
            if (enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.item.size = line.size();
                std::memcpy(cell.item.bytes.data(), line.data(), line.size());
                cell.sequence.store(pos + 1, std::memory_order_release);
                ++accepted;
                cv.notify_one();
                return true;
            }
        }
        return false;
    }
    void consume() noexcept {
        sigset_t blocked;
        ::sigemptyset(&blocked);
        ::sigaddset(&blocked, SIGPIPE);
        if (::pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0) {
            ++failures;
            return;
        }
        std::uint64_t reported = 0;
        try {
            for (;;) {
                if (expired()) break;
                Item item;
                if (pop(item)) {
                    if (write({ item.bytes.data(), item.size }))
                        ++written;
                    else {
                        ++failures;
                        ++dropped;
                        if (expired()) break;
                        fallback(dropped.load());
                    }
                } else if (stopping.load() && enqueue.load() == dequeue && dropped.load() == reported)
                    break;
                else {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait_for(lock, std::chrono::milliseconds(25));
                }
                auto lost = dropped.load();
                if (lost != reported) {
                    auto line = nlohmann::json{ { "utc", utc() }, { "level", "warning" }, { "event", "diagnostics_dropped" }, { "dropped", std::to_string(lost) } }.dump() + "\n";
                    if (!write(line)) {
                        ++failures;
                        fallback(lost);
                        if (expired()) break;
                    }
                    reported = lost;
                }
            }
        } catch (...) {
            ++failures;
        }
        dropped += enqueue.load() - dequeue;
        if (dropped.load() != reported) fallback(dropped.load());
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!stopping) {
                stopping = true;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
        }
        cv.notify_one();
        if (worker.joinable()) worker.join();
    }
};
LogSink::LogSink(int out, int err) : impl_(std::make_unique<Impl>(out, err)) {}
LogSink::~LogSink() = default;
void LogSink::stop() { impl_->stop(); }
LogSnapshot LogSink::snapshot() const noexcept { return { impl_->accepted.load(), impl_->dropped.load(), impl_->written.load(), impl_->failures.load() }; }
bool LogSink::tryEmit(const LogRecord& r) noexcept {
    try {
        using J = nlohmann::json;
        auto line = J{ { "utc", utc() }, { "level", r.level == LogLevel::Error ? "error" : r.level == LogLevel::Warning ? "warning"
                                                                                                                        : "info" },
                       { "event", name(r.event) },
                       { "request_id", r.requestId ? J(r.requestId->toString()) : J(nullptr) },
                       { "operation_id", r.operationId ? J(r.operationId->toString()) : J(nullptr) },
                       { "route", token(r.route, true) ? r.route : "unmatched" },
                       { "status", r.status },
                       { "code", token(r.code) ? r.code : "INTERNAL_ERROR" },
                       { "duration_us", std::to_string(r.durationUs) },
                       { "response_bytes_submitted", std::to_string(r.responseBytesSubmitted) },
                       { "outcome", token(r.outcome) ? r.outcome : "unknown" },
                       { "stage", token(r.stage) ? r.stage : "runtime" },
                       { "port", r.port } }
                        .dump() +
                    "\n";
        if (line.size() > 4096) {
            ++impl_->dropped;
            impl_->cv.notify_one();
            return false;
        }
        if (impl_->push(line)) return true;
        ++impl_->dropped;
        impl_->cv.notify_one();
        return false;
    } catch (...) {
        ++impl_->dropped;
        impl_->cv.notify_one();
        return false;
    }
}
}  // namespace anof::runtime
