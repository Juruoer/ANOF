#pragma once

#include <muduox/net/EventLoop.h>
#include <tests/diagnostics/WaitTrace.h>

#include <cmath>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>

namespace anof::test {
namespace detail {
enum class WaitOutcome { Pending,
                         Completed,
                         TimedOut,
                         Interrupted,
                         Failed };

// 首个被观察到的终态胜出；cancel/quit 不撤回同批回调，因此所有入口都检查终态。
struct WaitState {
    std::function<bool()> predicate;
    WaitTrace trace;
    WaitOutcome outcome = WaitOutcome::Pending;
    std::exception_ptr exception;

    void poll() {
        if (outcome != WaitOutcome::Pending) return;
        try {
            if (predicate()) {
                outcome = WaitOutcome::Completed;
                trace.event("done", true);
            }
        } catch (...) {
            exception = std::current_exception();
            outcome = WaitOutcome::Failed;
            trace.event("predicate_failed");
        }
    }
    void expire() {
        trace.event("deadline", outcome == WaitOutcome::Completed);
        if (outcome == WaitOutcome::Pending) outcome = WaitOutcome::TimedOut;
    }
};
}  // namespace detail

// 测试专用：所属 loop 的非回调栈内调用，不支持嵌套 loop。
// 默认仍为 1ms 轮询和 3s 截止；返回时不再次执行可能读 socket 的 predicate。
inline bool waitFor(muduox::net::EventLoop& loop, const std::function<bool()>& predicate,
                    double seconds = 3, const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
    loop.assertInLoopThread();
    if (!predicate || !std::isfinite(seconds) || seconds <= 0) throw std::invalid_argument("Invalid test wait");
    auto state = std::make_shared<detail::WaitState>(detail::WaitState{ predicate, WaitTrace(file, line), detail::WaitOutcome::Pending, {} });
    state->trace.event("begin");
    state->poll();
    if (state->exception) std::rethrow_exception(state->exception);
    if (state->outcome == detail::WaitOutcome::Completed) return true;
    {
        struct Timers {
            muduox::net::EventLoop& loop;
            std::shared_ptr<detail::WaitState> state;
            muduox::net::TimerId poll, deadline;
            ~Timers() {
                if (state->outcome == detail::WaitOutcome::Pending) state->outcome = detail::WaitOutcome::Interrupted;
                loop.cancel(poll);
                loop.cancel(deadline);
            }
        } timers{ loop, state, {}, {} };
        timers.poll = loop.runEvery(0.001, [state, &loop] {
            if (state->outcome != detail::WaitOutcome::Pending) return;
            state->poll();
            if (state->outcome != detail::WaitOutcome::Pending) loop.quit();
        });
        timers.deadline = loop.runAfter(seconds, [state, &loop] {
            const bool pending = state->outcome == detail::WaitOutcome::Pending;
            state->expire();
            if (pending) loop.quit();
        });
        loop.loop();
    }
    const bool completed = state->outcome == detail::WaitOutcome::Completed;
    state->trace.event(state->outcome == detail::WaitOutcome::TimedOut ? "expired" : "returned", completed);
    if (state->exception) std::rethrow_exception(state->exception);
    return completed;
}
}  // namespace anof::test
