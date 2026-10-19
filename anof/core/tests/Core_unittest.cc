#define BOOST_TEST_MODULE AnofCore
#include <boost/test/unit_test.hpp>

#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/core/Quantity.h>

#include <limits>
#include <memory>
#include <type_traits>

using namespace anof;

static_assert(!std::is_convertible_v<UserId, NodeId>);
static_assert(!std::is_convertible_v<ByteCount, NodeCount>);
static_assert(!std::is_default_constructible_v<NodeId>);
static_assert(!std::is_copy_constructible_v<Result<std::unique_ptr<int>>>);

BOOST_AUTO_TEST_CASE(uuid_canonical_and_invalid_inputs) {
    auto id = NodeId::parse("ABCDEF01-2345-4678-9abc-def012345678");
    BOOST_REQUIRE(id);
    BOOST_TEST(id.value().toString() == "abcdef01-2345-4678-9abc-def012345678");
    for (const auto* text : { "", "00000000-0000-0000-0000-000000000000",
                              "abcdef01_2345-4678-9abc-def012345678",
                              "abcdef01-2345-4678-9abc-def01234567g",
                              "abcdef01-2345-4678-9abc-def0123456789" }) {
        auto result = NodeId::parse(text);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::InvalidArgument);
    }
}

BOOST_AUTO_TEST_CASE(result_preserves_move_only_value_and_checks_access) {
    Result<std::unique_ptr<int>> first(std::make_unique<int>(42));
    auto second = std::move(first);
    BOOST_TEST(*second.value() == 42);
    auto value = std::move(second).value();
    BOOST_TEST(*value == 42);
    Result<int> failure(ErrorCode::CommitOutcomeUnknown);
    BOOST_CHECK(failure.error().code == ErrorCode::CommitOutcomeUnknown);
    BOOST_CHECK_THROW(failure.value(), std::bad_variant_access);
    Result<void> success;
    BOOST_REQUIRE(success);
    BOOST_CHECK_THROW(success.error(), std::bad_variant_access);
    Result<void> failedVoid(ErrorCode::Conflict);
    BOOST_CHECK_THROW(failedVoid.value(), std::bad_variant_access);
}

BOOST_AUTO_TEST_CASE(quantities_preserve_large_boundaries_and_reject_overflow) {
    for (std::int64_t boundary : { 1LL << 31, 1LL << 32, 1LL << 40 }) {
        auto value = ByteCount::from(boundary);
        BOOST_REQUIRE(value);
        BOOST_TEST(value.value().value() == boundary);
        BOOST_TEST(value.value().add(1).value().value() == boundary + 1);
    }
    BOOST_CHECK(!ByteCount::from(-1));
    BOOST_CHECK(!Revision::from(0));
    BOOST_TEST(Revision{}.value() == 1);
    const auto max = Revision::from(std::numeric_limits<std::int64_t>::max()).value();
    auto overflow = max.add(1);
    BOOST_REQUIRE(!overflow);
    BOOST_CHECK(overflow.error().code == ErrorCode::IntegrityError);
    BOOST_CHECK(!ByteCount{}.add(-1));
}

BOOST_AUTO_TEST_CASE(void_result_preserves_success_and_error_across_assignment) {
    Result<void> success;
    Result<void> failed(ErrorCode::StorageFull);
    success = failed;
    BOOST_REQUIRE(!success);
    BOOST_CHECK(success.error().code == ErrorCode::StorageFull);
    failed = Result<void>{};
    BOOST_REQUIRE(failed);
    BOOST_CHECK_THROW(failed.error(), std::bad_variant_access);
    auto moved = std::move(success);
    BOOST_REQUIRE(!moved);
    BOOST_CHECK(moved.error().code == ErrorCode::StorageFull);
    moved = std::move(failed);
    BOOST_REQUIRE(moved);
    BOOST_CHECK_NO_THROW(moved.value());
}

BOOST_AUTO_TEST_CASE(record_event_time_preserves_order_without_increment_or_overflow) {
    auto t = [](std::int64_t us) { return UtcTime{std::chrono::microseconds{us}}; };
    BOOST_CHECK(nondecreasingEventTime(t(9), t(10), t(12)) == t(12));
    BOOST_CHECK(nondecreasingEventTime(t(12), t(10), t(12)) == t(12));
    BOOST_CHECK(nondecreasingEventTime(t(14), t(10), t(12)) == t(14));
    BOOST_CHECK(nondecreasingEventTime(t(1), t(10), t(8)) == t(10));
    BOOST_CHECK(nondecreasingEventTime(t(std::numeric_limits<std::int64_t>::min()), t(0),
                                      t(std::numeric_limits<std::int64_t>::max())) ==
                t(std::numeric_limits<std::int64_t>::max()));
}
