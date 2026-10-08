#define BOOST_TEST_MODULE SystemClock
#include <boost/test/unit_test.hpp>

#include <anof/infrastructure/system/SystemClock.h>
#include <anof/infrastructure/system/internal/UtcTimeConversion.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

using namespace anof;
using namespace anof::infrastructure::system;
using NativeClock = std::chrono::system_clock;
using NativeDuration = NativeClock::duration;
using namespace std::chrono;

static_assert(std::is_same_v<decltype(std::declval<const Clock&>().now()), UtcTime>);
static_assert(std::is_same_v<UtcTime::duration, microseconds>);
// 当前 Linux 实现的纳秒刻度使边界期望值明确；不隐式截断测试输入。
static_assert(std::is_same_v<NativeDuration, nanoseconds>);
static_assert(std::numeric_limits<NativeDuration::rep>::digits == 63);

BOOST_AUTO_TEST_CASE(factory_and_production_clock) {
    auto clock = makeSystemClock();
    BOOST_REQUIRE(clock);
    BOOST_REQUIRE(clock.value());
    // 只确认生产调用可用，不依赖运行日期、睡眠或墙上时间递增。
    BOOST_CHECK_NO_THROW(static_cast<void>(clock.value()->now()));
}

BOOST_AUTO_TEST_CASE(epoch_and_submicrosecond_floor) {
    struct Sample { std::int64_t nanoseconds; std::int64_t microseconds; };
    const Sample samples[] = {
        { 0, 0 }, { 1, 0 }, { 999, 0 }, { 1000, 1 }, { 1001, 1 },
        { -1, -1 }, { -999, -1 }, { -1000, -1 }, { -1001, -2 },
        { -1999, -2 }, { -2000, -2 }
    };
    for (const auto& sample : samples) {
        const auto input = NativeClock::time_point(nanoseconds(sample.nanoseconds));
        BOOST_CHECK_EQUAL(detail::toUtcTime(input).time_since_epoch().count(), sample.microseconds);
    }
}

BOOST_AUTO_TEST_CASE(unix_epoch_large_values_and_backward_inputs) {
    struct Sample { std::int64_t nanoseconds; std::int64_t microseconds; };
    const Sample samples[] = {
        { 1700000000123456789LL, 1700000000123456LL },
        { -1700000000123456789LL, -1700000000123457LL },
        { 2147483648000000000LL, 2147483648000000LL },
        { 4294967296000000000LL, 4294967296000000LL },
        { 5000, 5 }, { 2000, 2 }, { -1, -1 }
    };
    for (const auto& sample : samples) {
        const auto input = NativeClock::time_point(nanoseconds(sample.nanoseconds));
        BOOST_CHECK_EQUAL(detail::toUtcTime(input).time_since_epoch().count(), sample.microseconds);
    }
}

BOOST_AUTO_TEST_CASE(native_representable_extremes) {
    const auto minimum = detail::toUtcTime(NativeClock::time_point::min());
    const auto maximum = detail::toUtcTime(NativeClock::time_point::max());
    BOOST_CHECK_EQUAL(minimum.time_since_epoch().count(), -9223372036854776LL);
    BOOST_CHECK_EQUAL(maximum.time_since_epoch().count(), 9223372036854775LL);
    // 最小值向下取整后的微秒不可再乘回纳秒；测试不作会溢出的反向转换。
    BOOST_CHECK_EQUAL(detail::toUtcTime(NativeClock::time_point(nanoseconds(-9223372036854775000LL))).time_since_epoch().count(), -9223372036854775LL);
    BOOST_CHECK_EQUAL(detail::toUtcTime(NativeClock::time_point(nanoseconds(9223372036854775000LL))).time_since_epoch().count(), 9223372036854775LL);
}
