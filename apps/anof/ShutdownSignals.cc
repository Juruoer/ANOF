#include <apps/anof/ShutdownSignals.h>

#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <exception>

namespace anof::app {
namespace {
std::atomic<bool> registered{ false };
}


Result<std::unique_ptr<ShutdownSignals>> ShutdownSignals::create() {
    try {
        auto result = std::unique_ptr<ShutdownSignals>(new ShutdownSignals);
        bool expected = false;
        if (!registered.compare_exchange_strong(expected, true)) return ErrorCode::Conflict;
        result->ownsRegistration_ = true;
        sigset_t signals;
        ::sigemptyset(&signals);
        ::sigaddset(&signals, SIGINT);
        ::sigaddset(&signals, SIGTERM);
        if (::pthread_sigmask(SIG_BLOCK, &signals, &result->original_) != 0) return ErrorCode::InternalError;
        result->maskInstalled_ = true;
        result->fd_ = ::signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
        if (result->fd_ < 0) return ErrorCode::InternalError;
        return result;
    } catch (...) { return ErrorCode::InternalError; }
}

ShutdownSignals::~ShutdownSignals() {
    if (owner_ != std::this_thread::get_id()) std::terminate();
    if (fd_ >= 0) {
        // 普通信号按种类合并，退出前消费当前已到达的至多两种信号。
        (void)poll();
        ::close(fd_);
    }
    if (maskInstalled_ && ::pthread_sigmask(SIG_SETMASK, &original_, nullptr) != 0) std::terminate();
    if (ownsRegistration_) registered = false;
}

Result<bool> ShutdownSignals::poll() {
    if (owner_ != std::this_thread::get_id()) return ErrorCode::InvalidArgument;
    std::array<signalfd_siginfo, 2> records{};
    const auto count = ::read(fd_, records.data(), sizeof records);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return requested_;
        return ErrorCode::InternalError;
    }
    if (count == 0 || static_cast<std::size_t>(count) % sizeof(signalfd_siginfo) != 0) return ErrorCode::InternalError;
    const auto size = static_cast<std::size_t>(count) / sizeof(signalfd_siginfo);
    for (std::size_t i = 0; i < size; ++i) {
        if (records[i].ssi_signo == SIGINT || records[i].ssi_signo == SIGTERM) requested_ = true;
    }
    return requested_;
}

}  // namespace anof::app
