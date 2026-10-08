#define BOOST_TEST_MODULE AnofQuota
#include <boost/test/unit_test.hpp>

#include <anof/domain/drive/Quota.h>

#include <limits>

using namespace anof;

namespace {
QuotaDelta amount(std::int64_t bytes, std::int64_t nodes) {
    return { ByteCount::from(bytes).value(), NodeCount::from(nodes).value() };
}
}

BOOST_AUTO_TEST_CASE(reserved_capacity_cannot_be_consumed_twice) {
    const QuotaState state{ amount(100, 10), amount(20, 7), amount(30, 2) };
    auto last = chargeUsed(state, amount(0, 1));
    BOOST_REQUIRE(last);
    BOOST_TEST(last.value().used.nodes.value() == 8);
    BOOST_TEST(last.value().reserved.nodes.value() == 2);
    auto excess = chargeUsed(last.value(), amount(0, 1));
    BOOST_REQUIRE(!excess);
    BOOST_CHECK(excess.error().code == ErrorCode::QuotaExceeded);
    BOOST_TEST(state.used.nodes.value() == 7);
    BOOST_CHECK(!chargeUsed(state, amount(51, 0)));
}

BOOST_AUTO_TEST_CASE(one_tib_and_invalid_persisted_accounting) {
    const auto tib = std::int64_t{ 1 } << 40;
    auto charged = chargeUsed({ amount(tib, 1), amount(0, 0), amount(0, 0) }, amount(tib, 1));
    BOOST_REQUIRE(charged);
    BOOST_TEST(charged.value().used.bytes.value() == tib);
    auto broken = chargeUsed({ amount(1, 1), amount(2, 0), amount(0, 0) }, amount(0, 0));
    BOOST_REQUIRE(!broken);
    BOOST_CHECK(broken.error().code == ErrorCode::IntegrityError);
    const auto max = std::numeric_limits<std::int64_t>::max();
    auto overflow = chargeUsed({ amount(max, 1), amount(max, 0), amount(1, 0) }, amount(0, 0));
    BOOST_REQUIRE(!overflow);
    BOOST_CHECK(overflow.error().code == ErrorCode::IntegrityError);
    auto addition = chargeUsed({ amount(max, 1), amount(max, 0), amount(0, 0) }, amount(1, 0));
    BOOST_REQUIRE(!addition);
    BOOST_CHECK(addition.error().code == ErrorCode::IntegrityError);
}
