#include <anof/runtime/CompletionPump.h>
#include <anof/runtime/DiskExecutor.h>
#include <muduox/net/EventLoop.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace anof::runtime {
namespace {
enum class Phase { Queued,
                   Cancelled,
                   Running,
                   Terminal };
struct Control {
    std::atomic<Phase> phase{ Phase::Queued };
    std::uint64_t id;
    explicit Control(std::uint64_t n) : id(n) {}
};
struct Handle final : TaskHandle {
    std::shared_ptr<Control> control;
    explicit Handle(std::shared_ptr<Control> c) : control(std::move(c)) {}
    std::uint64_t id() const noexcept override { return control->id; }
    CancelBeforeStartResult cancelBeforeStart() noexcept override {
        auto old = Phase::Queued;
        if (control->phase.compare_exchange_strong(old, Phase::Cancelled)) return CancelBeforeStartResult::Cancelled;
        return old == Phase::Running ? CancelBeforeStartResult::AlreadyRunning
                                     : CancelBeforeStartResult::AlreadyTerminal;
    }
};
struct Job {
    std::shared_ptr<Control> control;
    DiskExecutor::Operation operation;
    Completion<void> callback;
    TaskCompletion<void> result;
    bool cleanup;
    std::size_t lane;
};
struct Ring {
    std::vector<std::unique_ptr<Job>> entries;
    std::size_t head = 0, count = 0;
    explicit Ring(std::size_t size) : entries(size) {}
    void push(std::unique_ptr<Job> job) noexcept {
        entries[(head + count) % entries.size()] = std::move(job);
        ++count;
    }
    bool has(std::size_t lane) const noexcept {
        for (std::size_t i = 0; i < count; ++i)
            if (entries[(head + i) % entries.size()]->lane == lane) return true;
        return false;
    }
    std::unique_ptr<Job> take(std::size_t lane) noexcept {
        for (std::size_t i = 0; i < count; ++i)
            if (entries[(head + i) % entries.size()]->lane == lane) {
                auto job = std::move(entries[(head + i) % entries.size()]);
                for (auto j = i; j + 1 < count; ++j)
                    entries[(head + j) % entries.size()] = std::move(entries[(head + j + 1) % entries.size()]);
                --count;
                return job;
            }
        return {};
    }
    std::unique_ptr<Job> pop() noexcept {
        auto job = std::move(entries[head]);
        head = (head + 1) % entries.size();
        --count;
        return job;
    }
};
std::size_t capacity(DiskExecutorOptions options) {
    if (!options.normalPending || !options.cleanupPending || options.normalPending > 256 || options.cleanupPending > 8)
        throw std::invalid_argument("Invalid disk queue capacity");
    return options.normalPending + options.cleanupPending + 2;
}
}  // namespace
struct DiskExecutor::Impl {
    muduox::net::EventLoop& loop;
    DiskExecutorOptions options;
    mutable std::mutex mutex;
    std::condition_variable changed;
    Ring normal, cleanup, completed;
    std::size_t running = 0, outstanding = 0, normalOutstanding = 0, initialized = 0, finished = 0;
    bool started = false, ready = false, stopping = false, stopped = false, startDelivered = false;
    Result<void> startup;
    std::uint64_t nextId = 1;
    Started onStarted;
    Stopped onStopped;
    std::array<std::thread, 2> workers;
    std::array<std::promise<void>, 2> promises;
    std::array<std::future<void>, 2> exits;
    CompletionPump pump;
    Impl(muduox::net::EventLoop& l, DiskExecutorOptions o)
        : loop(l),
          options(o),
          normal(o.normalPending),
          cleanup(o.cleanupPending),
          completed(capacity(o)),
          pump(l, [this] { return drain(); }, [this] { finish(); }) {
        for (std::size_t i = 0; i < 2; ++i) exits[i] = promises[i].get_future();
    }
    void work(std::size_t index, Factory factory) noexcept {
        bool exitRegistered = false;
        try {
            promises[index].set_value_at_thread_exit();
            exitRegistered = true;
        } catch (...) {
        }
        {
            std::unique_ptr<TransferFileStore> store;
            Result<void> result;
            try {
                auto opened = factory();
                if (!opened)
                    result = opened.error();
                else if (!opened.value())
                    result = ErrorCode::InternalError;
                else
                    store = std::move(opened).value();
            } catch (...) {
                result = ErrorCode::InternalError;
            }
            factory = {};
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++initialized;
                if (!result) {
                    startup = result;
                    stopping = true;
                }
            }
            changed.notify_all();
            pump.notify();
            if (store)
                for (;;) {
                    std::unique_ptr<Job> job;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        changed.wait(lock,
                                     [this, index] { return stopping || cleanup.has(index) || normal.has(index); });
                        if (!cleanup.has(index) && !normal.has(index)) break;
                        job = cleanup.has(index) ? cleanup.take(index) : normal.take(index);
                        ++running;
                    }
                    auto expected = Phase::Queued;
                    if (job->control->phase.compare_exchange_strong(expected, Phase::Running)) {
                        try {
                            job->result.outcome = job->operation(*store);
                        } catch (...) {
                            job->result.outcome = Result<void>{ ErrorCode::CommitOutcomeUnknown };
                        }
                    } else
                        job->result.outcome = CancelledBeforeStart{};
                    job->control->phase.store(Phase::Terminal);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        --running;
                        completed.push(std::move(job));
                    }
                    pump.notify();
                }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++finished;
        }
        pump.notify();
        if (!exitRegistered) promises[index].set_value();
    }
    bool drain() noexcept {
        bool report = false;
        Result<void> result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (initialized == 2 && !startDelivered) {
                startDelivered = true;
                report = true;
                result = startup;
                ready = bool(result) && !stopping;
            }
        }
        if (report) {
            auto cb = std::move(onStarted);
            try {
                if (cb) cb(result);
            } catch (...) {
                stop();
            }
        }
        for (unsigned i = 0; i < 32; ++i) {
            std::unique_ptr<Job> job;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!completed.count) break;
                job = completed.pop();
            }
            const bool isCleanup = job->cleanup;
            try {
                job->callback(std::move(job->result));
            } catch (...) {
            }
            job.reset();
            {
                std::lock_guard<std::mutex> lock(mutex);
                --outstanding;
                if (!isCleanup) --normalOutstanding;
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        return completed.count != 0;
    }
    void finish() noexcept {
        if (!pump.idle()) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped || !stopping || finished != 2 || outstanding || !startDelivered) return;
        }
        for (auto& exit : exits)
            if (exit.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        for (auto& worker : workers)
            if (worker.joinable()) worker.join();
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
            ready = false;
        }
        auto cb = std::move(onStopped);
        try {
            if (cb) cb();
        } catch (...) {
        }
    }
    void stop() {
        loop.assertInLoopThread();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || stopped) return;
            stopping = true;
            ready = false;
            if (!started) {
                stopped = true;
                return;
            }
        }
        changed.notify_all();
        pump.notify();
    }
};
DiskExecutor::DiskExecutor(muduox::net::EventLoop& l, DiskExecutorOptions o) : impl_(std::make_unique<Impl>(l, o)) {}
DiskExecutor::~DiskExecutor() {
    for (auto& worker : impl_->workers)
        if (worker.joinable()) std::terminate();
}
Result<void> DiskExecutor::start(Factory factory, Started started, Stopped stopped) {
    auto& s = *impl_;
    s.loop.assertInLoopThread();
    if (!factory || s.started || s.stopped) return ErrorCode::InvalidArgument;
    s.started = true;
    s.onStarted = std::move(started);
    s.onStopped = std::move(stopped);
    std::size_t created = 0;
    try {
        for (; created < 2; ++created)
            s.workers[created] = std::thread([&s, factory, created] { s.work(created, factory); });
    } catch (...) {
        // 已创建线程仍异步退出；未创建线程登记为完成，统一交付启动失败和 stopped。
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.startup = ErrorCode::InternalError;
            s.stopping = true;
            for (auto i = created; i < 2; ++i) {
                ++s.initialized;
                ++s.finished;
                s.promises[i].set_value();
            }
        }
        s.changed.notify_all();
        s.pump.notify();
    }
    return {};
}
TaskSubmission DiskExecutor::submit(Operation operation, Completion<void> completion, bool cleanup, std::size_t lane) {
    auto& s = *impl_;
    s.loop.assertInLoopThread();
    try {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.ready || s.stopping) return SubmitError::Stopping;
        if (lane >= 2 || !operation || !completion || s.nextId == std::numeric_limits<std::uint64_t>::max())
            return SubmitError::InternalError;
        auto& queue = cleanup ? s.cleanup : s.normal;
        if (queue.count == queue.entries.size() || s.outstanding == s.completed.entries.size() ||
            (!cleanup && s.normalOutstanding == s.options.normalPending + 2))
            return SubmitError::QueueFull;
        auto control = std::make_shared<Control>(s.nextId);
        auto handle = std::make_unique<Handle>(control);
        auto job = std::make_unique<Job>(Job{ control,
                                              std::move(operation),
                                              std::move(completion),
                                              { s.nextId, Result<void>{ ErrorCode::InternalError } },
                                              cleanup,
                                              lane });
        queue.push(std::move(job));
        ++s.outstanding;
        ++s.nextId;
        if (!cleanup) ++s.normalOutstanding;
        s.changed.notify_all();
        return AcceptedTask{ std::move(handle) };
    } catch (...) {
        return SubmitError::InternalError;
    }
}
void DiskExecutor::requestStop() { impl_->stop(); }
DiskExecutorSnapshot DiskExecutor::snapshot() const {
    auto& s = *impl_;
    s.loop.assertInLoopThread();
    std::lock_guard<std::mutex> lock(s.mutex);
    return { s.normal.count + s.cleanup.count, s.running, s.outstanding, s.ready, s.stopped };
}
}  // namespace anof::runtime
