#define BOOST_TEST_MODULE EventLoopWait
#include <boost/test/unit_test.hpp>
#include <tests/support/EventLoopWait.h>

using namespace anof::test;
using namespace muduox::net;

BOOST_AUTO_TEST_CASE(completion_survives_deadline_in_same_collected_batch) {
    EventLoop loop;
    int calls = 0, callbacks = 0;
    detail::WaitState state{[&] { ++calls; return true; }, WaitTrace(__FILE__, __LINE__),
                            detail::WaitOutcome::Pending, {}};
    // 两个不同的过去期限保证顺序；都在首个 handleRead 的同一到期批次内，无需 sleep。
    loop.runAtMonotonic(MonotonicTime{}, [&] { ++callbacks; state.poll(); loop.quit(); });
    loop.runAtMonotonic(MonotonicTime{}.addSeconds(0.000001L), [&] {
        ++callbacks; state.expire(); state.poll();
    });
    loop.loop();
    BOOST_CHECK_EQUAL(callbacks, 2);
    BOOST_CHECK_EQUAL(calls, 1);
    BOOST_CHECK(state.outcome == detail::WaitOutcome::Completed);
}

BOOST_AUTO_TEST_CASE(deadline_wins_before_later_completion_in_same_batch) {
    EventLoop loop;
    int calls = 0, callbacks = 0;
    detail::WaitState state{[&] { ++calls; return true; }, WaitTrace(__FILE__, __LINE__),
                            detail::WaitOutcome::Pending, {}};
    loop.runAtMonotonic(MonotonicTime{}, [&] { ++callbacks; state.expire(); loop.quit(); });
    loop.runAtMonotonic(MonotonicTime{}.addSeconds(0.000001L), [&] { ++callbacks; state.poll(); });
    loop.loop();
    BOOST_CHECK_EQUAL(callbacks, 2);
    BOOST_CHECK_EQUAL(calls, 0);
    BOOST_CHECK(state.outcome == detail::WaitOutcome::TimedOut);
}

BOOST_AUTO_TEST_CASE(side_effecting_predicate_is_not_rechecked_after_success) {
    EventLoop loop;
    int calls = 0;
    BOOST_CHECK(waitFor(loop, [&] { return ++calls == 1; }));
    BOOST_CHECK_EQUAL(calls, 1);
    calls = 0;
    BOOST_CHECK(waitFor(loop, [&] { return ++calls == 2; }, 0.01));
    BOOST_CHECK_EQUAL(calls, 2);
    BOOST_CHECK(!waitFor(loop, [] { return false; }, 0.02));
    BOOST_CHECK_EQUAL(calls, 2); // 再次驱动 loop，上一轮定时器不能继续调用谓词。
}

BOOST_AUTO_TEST_CASE(external_quit_is_not_success_and_does_not_recheck_predicate) {
    EventLoop loop;
    bool done = false;
    int calls = 0;
    loop.queueInLoop([&] { done = true; loop.quit(); });
    BOOST_CHECK(!waitFor(loop, [&] { ++calls; return done; }, 0.01));
    const auto observed = calls;
    BOOST_CHECK_GE(observed, 1);
    BOOST_CHECK(!waitFor(loop, [] { return false; }, 0.02));
    BOOST_CHECK_EQUAL(calls, observed);
}

BOOST_AUTO_TEST_CASE(predicate_exception_is_rethrown_after_timer_cleanup) {
    EventLoop loop;
    int calls = 0;
    BOOST_CHECK_THROW(waitFor(loop, [&]() -> bool {
        if (++calls == 2) throw std::runtime_error("predicate failed");
        return false;
    }, 0.01), std::runtime_error);
    BOOST_CHECK_EQUAL(calls, 2);
    BOOST_CHECK(!waitFor(loop, [] { return false; }, 0.02));
    BOOST_CHECK_EQUAL(calls, 2);
}
