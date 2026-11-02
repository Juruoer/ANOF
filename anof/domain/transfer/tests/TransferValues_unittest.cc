#define BOOST_TEST_MODULE AnofTransferValues
#include <anof/application/ports/TransferDispatcher.h>
#include <anof/application/ports/TransferMetadataStore.h>
#include <anof/domain/drive/Quota.h>
#include <anof/domain/transfer/Storage.h>
#include <anof/domain/transfer/TransferPolicy.h>

#include <boost/test/unit_test.hpp>
#include <type_traits>

using namespace anof;
namespace {
ByteCount bytes(std::int64_t n) { return ByteCount::from(n).value(); }
QuotaDelta quota(std::int64_t b, std::int64_t n) { return { bytes(b), NodeCount::from(n).value() }; }
Upload sample() {
    const auto id = "11111111-1111-4111-8111-111111111111";
    UploadTarget target{ NodeId::parse(id).value(), "file" };
    const UtcTime now(std::chrono::microseconds(100));
    return { UploadId::parse(id).value(), UserId::parse(id).value(), OperationId::parse(id).value(), { SpaceId::parse(id).value(), target, FileLength::from(5).value(), std::nullopt }, target, {}, UploadState::Creating, {}, std::nullopt, std::nullopt, StagingLocation::Temporary, std::nullopt, now, now, uploadExpiry(now, now).value(), std::nullopt, std::nullopt };
}
}  // namespace
BOOST_AUTO_TEST_CASE(checked_numeric_boundaries_without_large_files) {
    BOOST_CHECK(FileLength::from(kMaxFileBytes));
    BOOST_CHECK(!FileLength::from(kMaxFileBytes + 1));
    BOOST_CHECK(!FileLength::from(-1));
    for (const auto* invalid : { "", "00", "01", "+1", "-1", " 1", "1 ", "1.0", "9223372036854775808" })
        BOOST_CHECK(!parseTransferDecimal(invalid));
    BOOST_CHECK(parseTransferDecimal("0"));
    BOOST_TEST(parseTransferDecimal("9223372036854775807").value().value() == INT64_MAX);
    auto length = FileLength::from(kMaxFileBytes).value();
    BOOST_TEST(checkedUploadEnd(length, UploadOffset::from(kMaxFileBytes - 1).value(), bytes(1)).value().value() == kMaxFileBytes);
    BOOST_CHECK(!checkedUploadEnd(length, UploadOffset::from(kMaxFileBytes).value(), bytes(INT64_MAX)));
    BOOST_CHECK(!checkedUploadEnd(length, UploadOffset::from(INT64_MAX).value(), bytes(0)));
    BOOST_CHECK(!checkedTransferInteger<std::int32_t>(bytes(kMaxFileBytes)));
    BOOST_CHECK(checkedTransferInteger<std::int64_t>(bytes(kMaxFileBytes)));
    BOOST_CHECK(!UploadGeneration::from(INT64_MAX).value().add(1));
}
BOOST_AUTO_TEST_CASE(digests_ids_and_ownership) {
    std::string hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    BOOST_TEST(Sha256::parse(hex).value().toString() == hex);
    BOOST_CHECK(!Sha256::parse(std::string(64, 'A')));
    BOOST_CHECK(!Sha256::parse(std::string(63, 'a')));
    BOOST_CHECK(!Sha256::parse(std::string(64, 'g')));
    static_assert(!std::is_same_v<UploadId, BlobId>);
    static_assert(!std::is_copy_constructible_v<OwnedTransferFile>);
    static_assert(!std::is_copy_constructible_v<OwnedTransferBlock>);
    static_assert(std::is_base_of_v<MetadataTransaction, TransferTransaction>);
    Blob blob{ BlobId::parse("abcdef12-1111-4111-8111-111111111111").value(), {}, Sha256::parse(hex).value() };
    BOOST_TEST(blob.storageKey() == "objects/ab/abcdef12-1111-4111-8111-111111111111");
}
BOOST_AUTO_TEST_CASE(disk_margin_and_overflow) {
    DiskSafetyPolicy policy{ bytes(0), SafetyPercent::from(5).value() };
    BOOST_TEST(diskSafetyMargin(bytes(101), policy).value().value() == 6);
    policy.percent = SafetyPercent::from(100).value();
    BOOST_TEST(diskSafetyMargin(bytes(INT64_MAX), policy).value().value() == INT64_MAX);
    BOOST_CHECK(!requireDiskCapacity(bytes(INT64_MAX), bytes(INT64_MAX), FileLength::from(1).value(), bytes(0)));
    BOOST_CHECK(requireDiskCapacity(bytes(10), bytes(3), FileLength::from(4).value(), bytes(3)));
    BOOST_CHECK(!requireDiskCapacity(bytes(9), bytes(3), FileLength::from(4).value(), bytes(3)));
}
BOOST_AUTO_TEST_CASE(reservations_are_conserved_and_underflow_is_rejected) {
    QuotaState q{ quota(100, 10), quota(10, 1), quota(0, 0) };
    auto reserved = reserveQuota(q, quota(90, 1));
    BOOST_REQUIRE(reserved);
    BOOST_CHECK(!reserveQuota(reserved.value(), quota(1, 0)));
    auto published = commitQuotaReservation(reserved.value(), quota(90, 1));
    BOOST_REQUIRE(published);
    BOOST_TEST(published.value().used.bytes.value() == 100);
    BOOST_TEST(published.value().reserved.bytes.value() == 0);
    BOOST_CHECK(!releaseQuotaReservation(published.value(), quota(1, 0)));
    auto released = releaseQuotaReservation(reserved.value(), quota(90, 1));
    BOOST_REQUIRE(released);
    BOOST_TEST(released.value().used.bytes.value() == 10);
    auto empty = reserveQuota(q, quota(0, 1));
    BOOST_REQUIRE(empty);
    BOOST_TEST(empty.value().reserved.nodes.value() == 1);
    BOOST_CHECK(!reserveQuota({ quota(INT64_MAX, 10), quota(INT64_MAX, 0), quota(1, 0) }, quota(0, 0)));
}
BOOST_AUTO_TEST_CASE(physical_reservation_survives_logical_cancellation) {
    StorageSnapshot initial{ bytes(100), bytes(10), bytes(0), Revision{} };
    auto held = reserveStorage(initial, bytes(90));
    BOOST_REQUIRE(held);
    BOOST_TEST(held.value().reserved.value() == 90);
    BOOST_TEST(held.value().revision.value() == 2);
    BOOST_CHECK(!reserveStorage(held.value(), bytes(1)));
    auto pub = publishStorage(held.value(), bytes(90));
    BOOST_REQUIRE(pub);
    BOOST_TEST(pub.value().retained.value() == 100);
    BOOST_TEST(pub.value().reserved.value() == 0);
    BOOST_CHECK(!publishStorage(pub.value(), bytes(1)));
    BOOST_CHECK(!setStorageBudget(held.value(), held.value().revision, bytes(99)));
    BOOST_TEST(setStorageBudget(held.value(), held.value().revision, bytes(100)).value().revision.value() == 2);
    BOOST_CHECK(!setStorageBudget(held.value(), Revision{}, bytes(200)));
    auto cleaned = releaseStorage(held.value(), bytes(90));
    BOOST_REQUIRE(cleaned);
    BOOST_TEST(cleaned.value().retained.value() == 10);
    BOOST_CHECK(!releaseStorage(cleaned.value(), bytes(90)));
    auto overflow = initial;
    overflow.revision = Revision::from(INT64_MAX).value();
    BOOST_CHECK(!reserveStorage(overflow, bytes(1)));
    // 超预算实例可清理，不允许新预留。
    held.value().budget = bytes(0);
    BOOST_CHECK(releaseStorage(held.value(), bytes(90)));
    BOOST_CHECK(!reserveStorage(held.value(), bytes(0)));
}
BOOST_AUTO_TEST_CASE(upload_invariants_transitions_and_retention) {
    auto u = sample();
    BOOST_REQUIRE(validateUpload(u));
    auto open = u;
    open.state = UploadState::Open;
    open.generation = u.generation.add(1).value();
    BOOST_CHECK(validateUploadReplacement(u, open));
    auto corrupt = open;
    corrupt.original.target.name = "changed";
    BOOST_CHECK(!validateUploadReplacement(u, corrupt));
    corrupt = open;
    corrupt.generation = u.generation;
    BOOST_CHECK(!validateUploadReplacement(u, corrupt));
    auto verified = open;
    verified.state = UploadState::Ready;
    verified.durableOffset = UploadOffset::from(5).value();
    BOOST_CHECK(!validateUpload(verified));
    verified.actualSha256 = Sha256::parse(std::string(64, 'a')).value();
    BOOST_CHECK(validateUpload(verified));
    BOOST_CHECK(!mayTransition(UploadState::Open, UploadState::Completed));
    BOOST_CHECK(!mayTransition(UploadState::Committing, UploadState::Expired));
    BOOST_CHECK(mayTransition(UploadState::Committing, UploadState::Ready));
    auto cancelled = open;
    cancelled.state = UploadState::Cancelled;
    cancelled.terminalAt = u.createdAt;
    cancelled.lastError = TransferFailure::UploadCancelled;
    cancelled.cleanupPending = true;
    cancelled.cancelRequested = true;
    cancelled.generation = open.generation.add(1).value();
    BOOST_CHECK(validateUploadReplacement(open, cancelled));
    auto lostCleanup = cancelled;
    lostCleanup.cleanupPending = false;
    BOOST_CHECK(!validateUploadReplacement(open, lostCleanup));
    const auto later = u.createdAt + std::chrono::hours(24 * 7);
    BOOST_CHECK(!mayForgetUpload(cancelled, later));
    auto cleaned = cancelled;
    cleaned.cleanupPending = false;
    cleaned.generation = cancelled.generation.add(1).value();
    BOOST_CHECK(validateUploadReplacement(cancelled, cleaned));
    BOOST_CHECK(mayForgetUpload(cleaned, later));
    BOOST_CHECK(!mayForgetUpload(cleaned, later - std::chrono::microseconds(1)));
    cleaned.cancelRequested = false;
    BOOST_CHECK(!validateUploadReplacement(cancelled, cleaned));
    BOOST_CHECK(!mayTransition(UploadState::Cancelled, UploadState::Open));
}
BOOST_AUTO_TEST_CASE(expiry_is_capped_and_checked) {
    UtcTime zero{};
    BOOST_CHECK(uploadExpiry(zero, zero).value() == zero + std::chrono::hours(24 * 7));
    BOOST_CHECK(uploadExpiry(zero, zero + std::chrono::hours(24 * 29)).value() == zero + std::chrono::hours(24 * 30));
    BOOST_CHECK(!uploadExpiry(UtcTime::max(), UtcTime::max()));
    BOOST_CHECK(!uploadExpiry(zero, zero - std::chrono::microseconds(1)));
}

BOOST_AUTO_TEST_CASE(record_limits_are_checked_without_flooding) {
    BOOST_CHECK(requireUploadCapacity({ 7, 63, 511, 4095 }));
    BOOST_CHECK(!requireUploadCapacity({ 8, 63, 511, 4095 }));
    BOOST_CHECK(!requireUploadCapacity({ 7, 64, 511, 4095 }));
    BOOST_CHECK(!requireUploadCapacity({ 7, 63, 512, 4095 }));
    BOOST_CHECK(!requireUploadCapacity({ 7, 63, 511, 4096 }));
    BOOST_CHECK(!requireUploadCapacity({ 7, 6, 511, 4095 }));
}
