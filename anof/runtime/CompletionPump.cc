#include <anof/runtime/internal/ShutdownTrace.h>
#include <anof/runtime/CompletionPump.h>

#include <muduox/net/EventLoop.h>
#include <muduox/net/TimerId.h>

#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace anof::runtime {

struct CompletionPump::State : std::enable_shared_from_this<State> {
    muduox::net::EventLoop& loop;
    Drain drain;
    Idle onIdle;
    Post post;
    muduox::net::TimerId recoveryTimer;
    mutable std::mutex mutex;
    bool scheduled = false;
    bool dirty = false;
    bool recovery = false;
    bool draining = false;
    std::size_t failures = 0;
    std::chrono::steady_clock::time_point lastRecovery = std::chrono::steady_clock::now();

    State(muduox::net::EventLoop& owner, Drain consume, Idle idleCallback, Post enqueue)
        : loop(owner), drain(std::move(consume)), onIdle(std::move(idleCallback)), post(std::move(enqueue)) {}

    void enqueue() noexcept {
        try {
            std::weak_ptr<State> weak = shared_from_this();
            post([weak] {
                if (auto self = weak.lock()) self->run();
            });
        } catch (...) {
            // 已接受的业务结果仍在预留槽位；固定恢复定时器不再依赖业务投递分配。
            std::lock_guard<std::mutex> lock(mutex);
            recovery = true;
            ++failures;
        }
    }

    void notify() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            dirty = true;
            if (scheduled) return;
            scheduled = true;
        }
        enqueue();
    }

    void run() noexcept {
        loop.assertInLoopThread();
        {
            std::lock_guard<std::mutex> lock(mutex);
            dirty = false;
            draining = true;
        }
        // drain 和 onIdle 为 runtime 内部回调，用户回调异常在宿主中隔离。
        const bool more = drain();
        bool again;
        {
            std::lock_guard<std::mutex> lock(mutex);
            draining = false;
            again = more || dirty;
            scheduled = again;
        }
        if (again) enqueue();
        else onIdle();
    }

    void recover() noexcept {
        if (internal::shutdownTraceEnabled()) {
            const auto now = std::chrono::steady_clock::now();
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRecovery).count();
            if (gap > 100) internal::shutdownTrace("pump", "recovery_gap_ms", this, static_cast<long>(gap));
            lastRecovery = now;
        }
        loop.assertInLoopThread();
        bool retry;
        bool isIdle;
        {
            std::lock_guard<std::mutex> lock(mutex);
            retry = recovery;
            recovery = false;
            isIdle = !scheduled && !draining;
        }
        if (retry) run();
        else if (isIdle) onIdle();  // 也用于观察真正的 thread-exit future，不阻塞 loop join。
    }
};

CompletionPump::CompletionPump(muduox::net::EventLoop& loop, Drain drain, Idle idleCallback, Post post) {
    loop.assertInLoopThread();
    if (!drain || !idleCallback) throw std::invalid_argument("CompletionPump requires callbacks");
    if (!post) post = [&loop](std::function<void()> callback) { loop.queueInLoop(std::move(callback)); };
    state_ = std::make_shared<State>(loop, std::move(drain), std::move(idleCallback), std::move(post));
    std::weak_ptr<State> weak = state_;
    internal::shutdownTrace("pump", "bind_state", this);
    internal::shutdownTrace("pump", "state", state_.get());
    state_->recoveryTimer = loop.runEvery(0.01, [weak] {
        if (auto self = weak.lock()) self->recover();
    });
}

CompletionPump::~CompletionPump() {
    state_->loop.assertInLoopThread();
    if (!idle()) std::terminate();
    state_->loop.cancel(state_->recoveryTimer);
}

void CompletionPump::notify() noexcept { state_->notify(); }

bool CompletionPump::idle() const {
    state_->loop.assertInLoopThread();
    std::lock_guard<std::mutex> lock(state_->mutex);
    return !state_->scheduled && !state_->draining;
}

std::size_t CompletionPump::postFailures() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->failures;
}

}  // namespace anof::runtime
