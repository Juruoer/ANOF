#define BOOST_TEST_MODULE ShutdownSignals
#include <apps/anof/ShutdownSignals.h>
#include <pthread.h>
#include <unistd.h>

#include <boost/test/unit_test.hpp>

namespace anof::app::testing {
BOOST_AUTO_TEST_CASE(blocked_signals_are_consumed_on_poll_and_request_is_sticky) {
    sigset_t before;
    BOOST_REQUIRE_EQUAL(::pthread_sigmask(SIG_SETMASK, nullptr, &before), 0);
    {
        auto result = ShutdownSignals::create();
        BOOST_REQUIRE(result);
        auto signals = std::move(result).value();
        auto duplicate = ShutdownSignals::create();
        BOOST_REQUIRE(!duplicate);
        BOOST_CHECK(duplicate.error().code == ErrorCode::Conflict);
        BOOST_REQUIRE(signals->poll());
        BOOST_CHECK(!signals->poll().value());
        sigset_t blocked;
        BOOST_REQUIRE_EQUAL(::pthread_sigmask(SIG_SETMASK, nullptr, &blocked), 0);
        BOOST_CHECK_EQUAL(::sigismember(&blocked, SIGINT), 1);
        BOOST_CHECK_EQUAL(::sigismember(&blocked, SIGTERM), 1);
        BOOST_REQUIRE_EQUAL(::kill(::getpid(), SIGTERM), 0);
        BOOST_REQUIRE_EQUAL(::kill(::getpid(), SIGINT), 0);
        BOOST_REQUIRE(signals->poll());
        BOOST_CHECK(signals->poll().value());
        BOOST_CHECK(signals->poll().value());
    }
    sigset_t after;
    BOOST_REQUIRE_EQUAL(::pthread_sigmask(SIG_SETMASK, nullptr, &after), 0);
    BOOST_CHECK_EQUAL(::sigismember(&after, SIGINT), ::sigismember(&before, SIGINT));
    BOOST_CHECK_EQUAL(::sigismember(&after, SIGTERM), ::sigismember(&before, SIGTERM));
}

BOOST_AUTO_TEST_CASE(worker_inherits_mask_and_cannot_consume_process_stop_request) {
    auto result = ShutdownSignals::create();
    BOOST_REQUIRE(result);
    auto signals = std::move(result).value();
    bool inherited = false, rejected = false;
    std::thread worker([&] {
        sigset_t blocked;
        const auto status = ::pthread_sigmask(SIG_SETMASK, nullptr, &blocked);
        inherited = status == 0 && ::sigismember(&blocked, SIGINT) == 1 && ::sigismember(&blocked, SIGTERM) == 1;
        const auto polled = signals->poll();
        rejected = !polled && polled.error().code == ErrorCode::InvalidArgument;
    });
    worker.join();
    BOOST_CHECK(inherited);
    BOOST_CHECK(rejected);
}
}  // namespace anof::app::testing
