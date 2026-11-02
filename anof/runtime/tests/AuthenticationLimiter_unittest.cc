#define BOOST_TEST_MODULE AuthenticationLimiter
#include <anof/runtime/AuthenticationLimiter.h>

#include <boost/test/unit_test.hpp>
using namespace anof;
using namespace anof::runtime;
BOOST_AUTO_TEST_CASE(pair_source_and_global_budgets_use_monotonic_refill) {
    AuthenticationLimiter limiter;
    const auto now = AuthenticationLimiter::Time{};
    const auto a = AuthenticationSource::ipv4({ 127, 0, 0, 1 });
    const auto b = AuthenticationSource::ipv4({ 127, 0, 0, 2 });
    const auto login = LoginName::parse("member").value();
    for (int i = 0; i < 3; ++i) BOOST_CHECK(limiter.allow(a, login, now));
    BOOST_CHECK(!limiter.allow(a, login, now));
    BOOST_CHECK(limiter.allow(b, login, now));
    BOOST_CHECK(limiter.allow(a, LoginName::parse("second").value(), now));
    BOOST_CHECK(limiter.allow(a, LoginName::parse("third").value(), now));
    BOOST_CHECK(!limiter.allow(a, LoginName::parse("fourth").value(), now));
    BOOST_CHECK(limiter.allow(a, login, now + std::chrono::seconds(12)));
    BOOST_CHECK(!limiter.allow(a, login, now));
    for (unsigned char i = 3; i < 12; ++i) BOOST_CHECK(limiter.allow(AuthenticationSource::ipv4({ 127, 0, 0, i }), login, now));
    // 全局初始16次，加12秒补充6次；上面时钟倒退被夹紧，不额外补充。
    AuthenticationLimiter global;
    for (unsigned char i = 1; i <= 16; ++i) BOOST_CHECK(global.allow(AuthenticationSource::ipv4({ 10, 0, 0, i }), login, now));
    BOOST_CHECK(!global.allow(AuthenticationSource::ipv4({ 10, 0, 0, 17 }), login, now));
}
BOOST_AUTO_TEST_CASE(bounded_entries_fail_closed_and_ipv4_has_one_representation) {
    AuthenticationLimiter limiter(2);
    auto now = AuthenticationLimiter::Time{};
    const auto a = AuthenticationSource::ipv4({ 127, 0, 0, 1 });
    const auto b = AuthenticationSource::ipv4({ 127, 0, 0, 2 });
    const auto login = LoginName::parse("member").value();
    BOOST_CHECK(limiter.allow(a, login, now));
    BOOST_CHECK(!limiter.allow(b, login, now));
    BOOST_CHECK(limiter.allow(b, login, now + std::chrono::minutes(10)));
    BOOST_CHECK(a == AuthenticationSource::ipv6({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 127, 0, 0, 1 }));
}
