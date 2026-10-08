#include <anof/runtime/AnofRuntime.h>

#include <muduox/net/EventLoop.h>

#include <exception>
#include <utility>

namespace anof::runtime {

AnofRuntime::AnofRuntime(muduox::net::EventLoop& loop, MetadataExecutorOptions options)
    : loop_(loop), metadata_(loop, options) {}

AnofRuntime::~AnofRuntime() {
    loop_.assertInLoopThread();
    if (started_ && state_ != RuntimeState::Stopped) std::terminate();
}

Result<void> AnofRuntime::start(MetadataExecutor::Factory factory, MetadataExecutor::Started callback,
                               MetadataExecutor::Stopped stopped) {
    loop_.assertInLoopThread();
    if (started_ || state_ != RuntimeState::Starting) return ErrorCode::InvalidArgument;
    onStopped_ = std::move(stopped);
    auto result = metadata_.start(std::move(factory), [this, callback = std::move(callback)](Result<void> initialized) {
        metadataReady_ = static_cast<bool>(initialized);
        if (!initialized) requestStop();
        if (callback) callback(initialized);
    }, [this] {
        metadataStopped_ = true;
        finishIfStopped();
    });
    if (!result) {
        // 同步启动失败没有 HTTP 或已接受任务，不伪造异步回调。
        metadata_.requestStop();
        state_ = RuntimeState::Stopped;
        onStopped_ = {};
        return result;
    }
    started_ = true;
    return {};
}

bool AnofRuntime::markHttpStarted() {
    loop_.assertInLoopThread();
    if (!started_ || !metadataReady_ || state_ != RuntimeState::Starting || httpStarted_) return false;
    httpStarted_ = true;
    state_ = RuntimeState::Ready;
    return true;
}

void AnofRuntime::requestStop() {
    loop_.assertInLoopThread();
    if (state_ == RuntimeState::Stopping || state_ == RuntimeState::Stopped) return;
    state_ = RuntimeState::Stopping;
    if (!httpStarted_) httpStopped_ = true;
    metadata_.requestStop();
    if (!started_) metadataStopped_ = true;
    finishIfStopped();
}

bool AnofRuntime::markHttpStopped() {
    loop_.assertInLoopThread();
    if (state_ != RuntimeState::Stopping || !httpStarted_ || httpStopped_) return false;
    httpStopped_ = true;
    finishIfStopped();
    return true;
}

void AnofRuntime::finishIfStopped() {
    if (state_ != RuntimeState::Stopping || !metadataStopped_ || !httpStopped_) return;
    state_ = RuntimeState::Stopped;
    auto callback = std::move(onStopped_);
    // 调用方不应抛异常；从 HTTP stop 路径进入时也须与 metadata 完成路径一样隔离。
    try { if (callback) callback(); }
    catch (...) { ++notificationExceptions_; }
}

RuntimeState AnofRuntime::state() const {
    loop_.assertInLoopThread();
    return state_;
}

bool AnofRuntime::ready() const { return state() == RuntimeState::Ready; }

CreateDirectoryDispatcher& AnofRuntime::dispatcher() {
    loop_.assertInLoopThread();
    return metadata_;
}

MetadataExecutorSnapshot AnofRuntime::metadataSnapshot() const { return metadata_.snapshot(); }

std::size_t AnofRuntime::notificationExceptions() const {
    loop_.assertInLoopThread();
    return notificationExceptions_;
}

}  // namespace anof::runtime
