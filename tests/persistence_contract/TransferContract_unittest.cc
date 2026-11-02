#define BOOST_TEST_MODULE AnofTransferContract
#include <tests/persistence_contract/TestTransferBackend.h>

#include <boost/test/unit_test.hpp>

namespace anof::testing {
namespace {
template <typename T>
T id() { return T::parse("11111111-1111-4111-8111-111111111111").value(); }
ByteCount bytes(std::int64_t n) { return ByteCount::from(n).value(); }
QuotaDelta amount(std::int64_t b, std::int64_t n) { return { bytes(b), NodeCount::from(n).value() }; }
struct Scenario {
    UtcTime now{ std::chrono::microseconds(1000000) };
    QuotaScope scope = SpaceQuota{ id<SpaceId>() };
    UploadTarget target{ id<NodeId>(), "file" };
    Upload upload{ id<UploadId>(), id<UserId>(), id<OperationId>(), { id<SpaceId>(), target, FileLength::from(16).value(), std::nullopt }, target, {}, UploadState::Creating, {}, std::nullopt, std::nullopt, StagingLocation::Temporary, std::nullopt, now, now, uploadExpiry(now, now).value(), std::nullopt, std::nullopt };
    TransferSeed seed{
        { { { { id<UserId>(), UserRole::Administrator, AuthEpoch{} }, true } }, {}, { { id<SpaceId>(), SpaceKind::Public, std::nullopt, id<NodeId>() } }, {}, { { { id<NodeId>(), id<SpaceId>(), std::nullopt, "", id<UserId>(), Revision{}, now, now }, std::nullopt, false } }, { { scope, { amount(32, 10), amount(0, 1), amount(0, 0) } } } },
        { bytes(32), bytes(0), bytes(0), Revision{} },
        {}
    };
    TransferEvent event() const { return { TransferEventKind::Create, id<RequestId>(), id<UserId>(), upload.id, now, std::nullopt }; }
};
std::unique_ptr<TestTransferBackend> backendFor(const TransferSeed& seed) {
    auto backend = makeTestTransferBackend();
    BOOST_REQUIRE(backend);
    BOOST_REQUIRE(backend->reset(seed));
    return backend;
}
std::unique_ptr<TransferMetadataStore> connect(TestTransferBackend& backend) {
    auto result = backend.connect();
    BOOST_REQUIRE(result);
    BOOST_REQUIRE(result.value());
    return std::move(result).value();
}
std::unique_ptr<TransferTransaction> begin(TransferMetadataStore& store) {
    auto result = store.beginTransferWrite();
    BOOST_REQUIRE(result);
    BOOST_REQUIRE(result.value());
    return std::move(result).value();
}
std::unique_ptr<TransferReadSnapshot> read(TransferMetadataStore& store) {
    auto result = store.beginTransferRead();
    BOOST_REQUIRE(result);
    BOOST_REQUIRE(result.value());
    return std::move(result).value();
}
void create(TransferTransaction& tx, const Scenario& s) {
    BOOST_REQUIRE(tx.quotas().reserve(s.scope, amount(16, 1)));
    BOOST_REQUIRE(tx.storage().reserve(bytes(16)));
    BOOST_REQUIRE(tx.uploads().insert(s.upload));
    BOOST_REQUIRE(tx.transferAudit().append(s.event()));
}
}  // namespace
BOOST_AUTO_TEST_CASE(creation_rollback_and_shared_commit) {
    Scenario s;
    auto backend = backendFor(s.seed);
    auto store = connect(*backend);
    {
        auto tx = begin(*store);
        create(*tx, s);
    }
    {
        auto snap = read(*store);
        auto row = snap->uploads().find(s.upload.id);
        BOOST_REQUIRE(row);
        BOOST_CHECK(!row.value());
        auto quota = snap->quotas().get(s.scope);
        BOOST_REQUIRE(quota);
        BOOST_TEST(quota.value().state.reserved.bytes.value() == 0);
        auto storage = snap->storage().get();
        BOOST_REQUIRE(storage);
        BOOST_TEST(storage.value().reserved.value() == 0);
    }
    auto audit = backend->readTransferAudit();
    BOOST_REQUIRE(audit);
    BOOST_TEST(audit.value().size() == 0U);
    {
        auto tx = begin(*store);
        create(*tx, s);
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto snap = read(*store);
        auto row = snap->uploads().findByClientId(s.upload.userId, s.upload.clientUploadId);
        BOOST_REQUIRE(row);
        BOOST_REQUIRE(row.value());
        BOOST_CHECK(row.value()->id == s.upload.id);
        auto q = snap->quotas().get(s.scope);
        BOOST_REQUIRE(q);
        BOOST_TEST(q.value().state.reserved.bytes.value() == 16);
        auto st = snap->storage().get();
        BOOST_REQUIRE(st);
        BOOST_TEST(st.value().reserved.value() == 16);
    }
    audit = backend->readTransferAudit();
    BOOST_REQUIRE(audit);
    BOOST_TEST(audit.value().size() == 1U);
}
BOOST_AUTO_TEST_CASE(stale_generation_cannot_overwrite_disable_cancellation) {
    Scenario s;
    auto backend = backendFor(s.seed);
    auto store = connect(*backend);
    {
        auto tx = begin(*store);
        create(*tx, s);
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto tx = begin(*store);
        auto affected = tx->uploads().requestUserCancellation(s.upload.userId);
        BOOST_REQUIRE(affected);
        BOOST_TEST(affected.value().size() == 1U);
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto tx = begin(*store);
        auto next = s.upload;
        next.state = UploadState::Open;
        next.generation = next.generation.add(1).value();
        auto result = tx->uploads().replace(next.id, { s.upload.state, s.upload.generation, s.upload.durableOffset }, next);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::Conflict);
    }
    auto snap = read(*store);
    auto row = snap->uploads().find(s.upload.id);
    BOOST_REQUIRE(row);
    BOOST_REQUIRE(row.value());
    BOOST_CHECK(row.value()->cancelRequested);
    BOOST_TEST(row.value()->generation.value() == 2);
}
BOOST_AUTO_TEST_CASE(logical_cancel_retains_physical_commitment_until_cleanup_transaction) {
    Scenario s;
    auto backend = backendFor(s.seed);
    auto store = connect(*backend);
    {
        auto tx = begin(*store);
        create(*tx, s);
        BOOST_REQUIRE(tx->commit());
    }
    auto cancelled = s.upload;
    cancelled.state = UploadState::Cancelled;
    cancelled.generation = cancelled.generation.add(1).value();
    cancelled.terminalAt = s.now;
    cancelled.cleanupPending = true;
    cancelled.lastError = TransferFailure::UploadCancelled;
    {
        auto tx = begin(*store);
        BOOST_REQUIRE(tx->uploads().replace(cancelled.id, { s.upload.state, s.upload.generation, s.upload.durableOffset }, cancelled));
        BOOST_REQUIRE(tx->quotas().releaseReservation(s.scope, amount(16, 1)));
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto snap = read(*store);
        auto q = snap->quotas().get(s.scope);
        BOOST_REQUIRE(q);
        BOOST_TEST(q.value().state.reserved.bytes.value() == 0);
        auto st = snap->storage().get();
        BOOST_REQUIRE(st);
        BOOST_TEST(st.value().reserved.value() == 16);
    }
    auto cleaned = cancelled;
    cleaned.cleanupPending = false;
    cleaned.generation = cleaned.generation.add(1).value();
    {
        auto tx = begin(*store);
        BOOST_REQUIRE(tx->uploads().replace(cleaned.id, { cancelled.state, cancelled.generation, cancelled.durableOffset }, cleaned));
        BOOST_REQUIRE(tx->storage().releaseReservation(bytes(16)));
        // 模拟清理确认事务失败：回滚必须同时恢复责任与 R。
    }
    {
        auto snap = read(*store);
        auto row = snap->uploads().find(cleaned.id);
        BOOST_REQUIRE(row);
        BOOST_REQUIRE(row.value());
        BOOST_CHECK(row.value()->cleanupPending);
        auto st = snap->storage().get();
        BOOST_REQUIRE(st);
        BOOST_TEST(st.value().reserved.value() == 16);
    }
    {
        auto tx = begin(*store);
        BOOST_REQUIRE(tx->uploads().replace(cleaned.id, { cancelled.state, cancelled.generation, cancelled.durableOffset }, cleaned));
        BOOST_REQUIRE(tx->storage().releaseReservation(bytes(16)));
        BOOST_REQUIRE(tx->commit());
    }
    auto snap = read(*store);
    auto st = snap->storage().get();
    BOOST_REQUIRE(st);
    BOOST_TEST(st.value().reserved.value() == 0);
}
}  // namespace anof::testing
