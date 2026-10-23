#define BOOST_TEST_MODULE SqliteTransfer
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestTransferBackend.h>
#include <sqlite3.h>

#include <boost/test/unit_test.hpp>
#include <cstdio>
namespace anof::infrastructure::sqlite::testing {
namespace {
template <class T>
T id(unsigned n = 1) {
    char text[37];
    std::snprintf(text, sizeof(text), "12345678-1234-4234-8234-%012x", n);
    return T::parse(text).value();
}
ByteCount bytes(std::int64_t n) { return ByteCount::from(n).value(); }
QuotaDelta amount(std::int64_t b, std::int64_t n) { return { bytes(b), NodeCount::from(n).value() }; }
UtcTime now{ std::chrono::microseconds{ 1000000 } };
Upload upload(unsigned n = 1, unsigned user = 1) {
    UploadTarget target{ id<NodeId>(), "file" };
    return { id<UploadId>(n), id<UserId>(user), id<OperationId>(n), { id<SpaceId>(), target, FileLength::from(16).value(), {} }, target, {}, UploadState::Creating, {}, {}, {}, StagingLocation::Temporary, {}, now, now, uploadExpiry(now, now).value(), {}, {} };
}
anof::testing::TransferSeed seed() {
    QuotaScope scope = SpaceQuota{ id<SpaceId>() };
    return { { { { { id<UserId>(), UserRole::Administrator, {} }, true } }, {}, { { id<SpaceId>(), SpaceKind::Public, {}, id<NodeId>() } }, {}, { { { id<NodeId>(), id<SpaceId>(), {}, "", id<UserId>(), {}, now, now }, {}, false } }, { { scope, { amount(100000, 10000), amount(0, 1), amount(0, 0) } }, { PublicContributionQuota{ id<SpaceId>(), id<UserId>() }, { amount(100000, 10000), amount(0, 1), amount(0, 0) } } } },
             { bytes(100000), bytes(0), bytes(0), {} },
             {} };
}
template <class T>
T take(Result<T> r) {
    BOOST_REQUIRE(r);
    return std::move(r).value();
}
template <class T>
void errorIs(const Result<T>& r, ErrorCode e) {
    BOOST_REQUIRE(!r);
    BOOST_CHECK(r.error().code == e);
}
struct Fixture {
    SqliteTestTransferBackend backend;
    std::unique_ptr<TransferMetadataStore> store;
    explicit Fixture(anof::testing::TransferSeed s = seed()) {
        BOOST_REQUIRE(backend.reset(s));
        store = take(backend.connect());
    }
    auto write() { return take(store->beginTransferWrite()); }
    auto read() { return take(store->beginTransferRead()); }
    auto raw() { return take(SqliteConnection::open(backend.databasePath(), { 0 }, SqliteOpenMode::Inspect)); }
};
Upload advance(Upload u, UploadState state) {
    u.generation = u.generation.add(1).value();
    u.state = state;
    return u;
}
void replace(TransferTransaction& tx, const Upload& before, const Upload& after) { BOOST_REQUIRE(tx.uploads().replace(before.id, { before.state, before.generation, before.durableOffset }, after)); }
}  // namespace
BOOST_AUTO_TEST_CASE(base_and_transfer_share_transaction_and_snapshot_lifetimes) {
    Fixture f;
    auto other = take(f.backend.connect());
    auto tx = f.write();
    errorIs(f.store->beginRead(), ErrorCode::InvalidArgument);
    errorIs(f.store->beginWrite(), ErrorCode::InvalidArgument);
    errorIs(f.store->beginTransferRead(), ErrorCode::InvalidArgument);
    errorIs(other->beginTransferWrite(), ErrorCode::Retryable);
    BOOST_REQUIRE(tx->uploads().insert(upload()));
    auto snapshot = take(other->beginTransferRead());
    BOOST_CHECK(!take(snapshot->uploads().find(id<UploadId>())));
    BOOST_CHECK(dynamic_cast<UploadRepository*>(&snapshot->uploads()) == nullptr);
    BOOST_REQUIRE(tx->commit());
    BOOST_CHECK(!take(snapshot->uploads().find(id<UploadId>())));
    snapshot.reset();
    snapshot = take(other->beginTransferRead());
    BOOST_REQUIRE(take(snapshot->uploads().find(id<UploadId>())));
    errorIs(tx->uploads().counts(id<UserId>()), ErrorCode::InvalidArgument);
}
BOOST_AUTO_TEST_CASE(capacity_uniqueness_and_bounded_seek) {
    Fixture f;
    {
        auto tx = f.write();
        for (unsigned i = 1; i <= 8; ++i) BOOST_REQUIRE(tx->uploads().insert(upload(i)));
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto tx = f.write();
        errorIs(tx->uploads().insert(upload(9)), ErrorCode::UploadLimitReached);
        BOOST_CHECK(!tx->commit());
    }
    {
        auto tx = f.write();
        errorIs(tx->uploads().insert(upload()), ErrorCode::Conflict);
    }
    auto snapshot = f.read();
    auto counts = take(snapshot->uploads().counts(id<UserId>()));
    BOOST_CHECK_EQUAL(counts.activeUser, 8);
    std::optional<UploadPosition> after;
    unsigned count = 0;
    do {
        auto page = take(snapshot->uploads().list(id<UserId>(), PageSize::from(3).value(), after));
        for (const auto& u : page.items) {
            BOOST_CHECK(u.id == id<UploadId>(++count));
        }
        after = page.next;
    } while (after && count <= 8);
    BOOST_CHECK_EQUAL(count, 8);
    BOOST_CHECK(!after);
    BOOST_CHECK_EQUAL(take(snapshot->uploads().unwrittenCommitment()).value(), 128);
}
BOOST_AUTO_TEST_CASE(global_and_retained_limits_do_not_evict_history) {
    for (unsigned mode = 0; mode < 3; ++mode) {
        auto s = seed();
        unsigned users = mode == 0 ? 9 : mode == 1 ? 1
                                                   : 9;
        for (unsigned n = 2; n <= users; ++n) s.metadata.users.push_back({ { id<UserId>(n), UserRole::Member, {} }, true });
        const unsigned count = mode == 0 ? 64 : mode == 1 ? 512
                                                          : 4096;
        for (unsigned i = 1; i <= count; ++i) {
            auto u = upload(i, mode == 1 ? 1 : (i - 1) / (mode == 0 ? 8 : 512) + 1);
            if (mode) {
                u.state = UploadState::Cancelled;
                u.terminalAt = now;
            }
            s.uploads.push_back(u);
        }
        Fixture f(s);
        auto tx = f.write();
        errorIs(tx->uploads().insert(upload(count + 1, users)), ErrorCode::UploadLimitReached);
        BOOST_CHECK(!tx->commit());
        auto read = f.read();
        BOOST_CHECK_EQUAL(take(read->uploads().counts(id<UserId>())).retainedGlobal, count);
    }
}
BOOST_AUTO_TEST_CASE(generation_cancellation_and_retention_boundaries) {
    auto s = seed();
    auto committing = upload();
    committing.state = UploadState::Committing;
    committing.durableOffset = UploadOffset::from(16).value();
    committing.actualSha256 = Sha256({});
    committing.pendingBlobId = id<BlobId>();
    s.uploads = { committing };
    Fixture f(s);
    {
        auto tx = f.write();
        auto ids = take(tx->uploads().requestUserCancellation(id<UserId>()));
        BOOST_CHECK_EQUAL(ids.size(), 1);
        BOOST_CHECK(take(tx->uploads().requestUserCancellation(id<UserId>())).empty());
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto read = f.read();
        auto row = take(read->uploads().find(committing.id));
        BOOST_REQUIRE(row);
        BOOST_CHECK(row->cancelRequested);
        BOOST_CHECK_EQUAL(row->generation.value(), 2);
        auto expired = take(read->uploads().scan(UploadScan::Expired, committing.expiresAt, PageSize{}, {}));
        BOOST_CHECK(expired.items.empty());
    }
    committing.cancelRequested = true;
    committing.generation = UploadGeneration::from(2).value();
    auto cancelled = advance(committing, UploadState::Cancelled);
    cancelled.terminalAt = now;
    cancelled.cleanupPending = true;
    {
        auto tx = f.write();
        replace(*tx, committing, cancelled);
        BOOST_REQUIRE(tx->commit());
    }
    auto later = now + std::chrono::hours(24 * 7);
    {
        auto tx = f.write();
        errorIs(tx->uploads().eraseTerminal(cancelled.id, cancelled.generation, later), ErrorCode::Conflict);
    }
    auto clean = advance(cancelled, UploadState::Cancelled);
    clean.cleanupPending = false;
    {
        auto tx = f.write();
        replace(*tx, cancelled, clean);
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto tx = f.write();
        errorIs(tx->uploads().eraseTerminal(clean.id, clean.generation, later - std::chrono::microseconds(1)), ErrorCode::Conflict);
    }
    {
        auto read = f.read();
        BOOST_CHECK_EQUAL(take(read->uploads().scan(UploadScan::RetentionElapsed, later, PageSize{}, {})).items.size(), 1);
    }
    {
        auto tx = f.write();
        BOOST_REQUIRE(tx->uploads().eraseTerminal(clean.id, clean.generation, later));
        BOOST_REQUIRE(tx->commit());
    }
    BOOST_CHECK(!take(f.read()->uploads().find(clean.id)));
}
BOOST_AUTO_TEST_CASE(invalid_replacements_and_overflow_roll_back_all_cancellation_marks) {
    auto s = seed();
    s.uploads = { upload(1), upload(2) };
    s.uploads.back().generation = UploadGeneration::from(INT64_MAX).value();
    Fixture f(s);
    {
        auto tx = f.write();
        errorIs(tx->uploads().requestUserCancellation(id<UserId>()), ErrorCode::IntegrityError);
        BOOST_CHECK(!tx->commit());
    }
    {
        auto read = f.read();
        BOOST_CHECK(!take(read->uploads().find(id<UploadId>()))->cancelRequested);
    }
    auto changed = advance(upload(), UploadState::Open);
    changed.original.target.name = "different";
    {
        auto tx = f.write();
        errorIs(tx->uploads().replace(changed.id, { UploadState::Creating, {}, {} }, changed), ErrorCode::IntegrityError);
    }
    BOOST_REQUIRE(f.raw()->execute("PRAGMA ignore_check_constraints=ON; UPDATE uploads SET state=99,generation=generation+1 WHERE generation=1"));
    {
        auto read = f.read();
        errorIs(read->uploads().find(id<UploadId>()), ErrorCode::IntegrityError);
    }
}
BOOST_AUTO_TEST_CASE(publication_moves_reservations_and_exposes_immutable_blob_metadata) {
    Fixture f;
    const QuotaScope space = SpaceQuota{ id<SpaceId>() };
    const QuotaScope contribution = PublicContributionQuota{ id<SpaceId>(), id<UserId>() };
    Blob blob{ id<BlobId>(), FileLength::from(16).value(), Sha256({}) };
    {
        auto tx = f.write();
        for (const auto& scope : { space, contribution }) BOOST_REQUIRE(tx->quotas().reserve(scope, amount(16, 1)));
        BOOST_REQUIRE(tx->storage().reserve(bytes(16)));
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto tx = f.write();
        BOOST_REQUIRE(tx->blobs().insert({ blob, BlobState::Available, now, now, {}, {}, {} }));
        BOOST_REQUIRE(tx->storage().publishReservation(bytes(16)));
        for (const auto& scope : { space, contribution }) BOOST_REQUIRE(tx->quotas().commitReservation(scope, amount(16, 1)));
        auto node = take(tx->nodes().insertFile({ { id<NodeId>(2), id<SpaceId>(), id<NodeId>(), "file", id<UserId>(), now }, blob.id }));
        BOOST_CHECK(std::holds_alternative<FileContentMetadata>(node.content));
        BOOST_REQUIRE(tx->nodes().advanceRevision(id<NodeId>(), Revision{}, now));
        BOOST_REQUIRE(tx->transferAudit().append({ TransferEventKind::Publish, id<RequestId>(), id<UserId>(), id<UploadId>(), now, {} }));
        BOOST_REQUIRE(tx->commit());
    }
    auto read = f.read();
    auto storage = take(read->storage().get());
    BOOST_CHECK_EQUAL(storage.retained.value(), 16);
    BOOST_CHECK_EQUAL(storage.reserved.value(), 0);
    BOOST_CHECK(take(read->blobs().hasNodeReference(blob.id)));
    BOOST_CHECK(!take(read->blobs().hasUploadReference(blob.id)));
    BOOST_REQUIRE(take(read->blobs().findByNode(id<NodeId>(2))));
    for (const auto& scope : { space, contribution }) {
        auto quota = take(read->quotas().get(scope));
        BOOST_CHECK_EQUAL(quota.state.used.nodes.value(), 2);
        BOOST_CHECK_EQUAL(quota.state.reserved.nodes.value(), 0);
    }
}
BOOST_AUTO_TEST_CASE(storage_budget_overflow_and_cleanup_semantics) {
    auto s = seed();
    s.storage = { bytes(1), bytes(10), bytes(16), Revision{} };
    Fixture f(s);
    {
        auto tx = f.write();
        errorIs(tx->storage().reserve(bytes(0)), ErrorCode::StorageBudgetExceeded);
    }
    {
        auto tx = f.write();
        errorIs(tx->storage().setBudget(Revision::from(2).value(), bytes(100)), ErrorCode::PreconditionFailed);
    }
    {
        auto tx = f.write();
        errorIs(tx->storage().setBudget(Revision{}, bytes(25)), ErrorCode::StorageBudgetExceeded);
    }
    {
        auto tx = f.write();
        BOOST_REQUIRE(tx->storage().releaseReservation(bytes(0)));
        BOOST_CHECK_EQUAL(take(tx->storage().get()).revision.value(), 1);
        BOOST_REQUIRE(tx->storage().releaseReservation(bytes(16)));
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto tx = f.write();
        errorIs(tx->storage().releaseReservation(bytes(1)), ErrorCode::IntegrityError);
    }
    BOOST_REQUIRE(f.raw()->execute("UPDATE storage_accounting SET revision=9223372036854775807"));
    {
        auto tx = f.write();
        errorIs(tx->storage().setBudget(Revision::from(INT64_MAX).value(), bytes(100)), ErrorCode::IntegrityError);
    }
    BOOST_CHECK_EQUAL(take(f.read()->storage().get()).budget.value(), 1);
}
BOOST_AUTO_TEST_CASE(audit_failure_rolls_back_reservation_and_file_publication) {
    Fixture f;
    auto tx = f.write();
    auto raw = f.raw();
    // 故障触发器在连接校验之后注入，所有修改仍走生产仓储。
    tx.reset();
    BOOST_REQUIRE(raw->execute("CREATE TRIGGER fail_transfer_audit BEFORE INSERT ON transfer_audit_events BEGIN SELECT RAISE(ABORT,'injected'); END"));
    tx = f.write();
    BOOST_REQUIRE(tx->uploads().insert(upload()));
    BOOST_REQUIRE(tx->storage().reserve(bytes(16)));
    BOOST_REQUIRE(tx->quotas().reserve(SpaceQuota{ id<SpaceId>() }, amount(16, 1)));
    Blob blob{ id<BlobId>(), FileLength::from(16).value(), Sha256({}) };
    BOOST_REQUIRE(tx->blobs().insert({ blob, BlobState::Available, now, now, {}, {}, {} }));
    BOOST_REQUIRE(tx->nodes().insertFile({ { id<NodeId>(2), id<SpaceId>(), id<NodeId>(), "file", id<UserId>(), now }, blob.id }));
    BOOST_REQUIRE(tx->nodes().advanceRevision(id<NodeId>(), Revision{}, now));
    BOOST_REQUIRE(tx->quotas().commitReservation(SpaceQuota{ id<SpaceId>() }, amount(16, 1)));
    BOOST_REQUIRE(tx->storage().publishReservation(bytes(16)));
    errorIs(tx->transferAudit().append({ TransferEventKind::Publish, id<RequestId>(), id<UserId>(), id<UploadId>(), now, {} }), ErrorCode::IntegrityError);
    BOOST_CHECK(!tx->commit());
    auto read = f.read();
    BOOST_CHECK(!take(read->uploads().find(id<UploadId>())));
    BOOST_CHECK_EQUAL(take(read->storage().get()).reserved.value(), 0);
    BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 0);
    BOOST_CHECK(!take(read->blobs().find(blob.id)));
    BOOST_CHECK(!take(read->blobs().findByNode(id<NodeId>(2))));
    BOOST_CHECK_EQUAL(take(read->quotas().get(SpaceQuota{ id<SpaceId>() })).state.used.nodes.value(), 1);
}
BOOST_AUTO_TEST_CASE(reservations_serialize_and_budget_changes_compare_current_revision) {
    auto s = seed();
    s.storage.budget = bytes(16);
    Fixture f(s);
    auto other = take(f.backend.connect());
    auto tx = f.write();
    BOOST_REQUIRE(tx->storage().reserve(bytes(16)));
    errorIs(other->beginTransferWrite(), ErrorCode::Retryable);
    BOOST_REQUIRE(tx->commit());
    auto next = take(other->beginTransferWrite());
    errorIs(next->storage().reserve(bytes(1)), ErrorCode::StorageBudgetExceeded);
    BOOST_CHECK(!next->commit());
    next = take(other->beginTransferWrite());
    errorIs(next->storage().setBudget(Revision{}, bytes(15)), ErrorCode::PreconditionFailed);
    BOOST_CHECK(!next->commit());
    next = take(other->beginTransferWrite());
    BOOST_REQUIRE(next->storage().releaseReservation(bytes(16)));
    BOOST_REQUIRE(next->storage().setBudget(Revision::from(3).value(), bytes(8)));
    BOOST_REQUIRE(next->commit());
    tx = f.write();
    errorIs(tx->storage().reserve(bytes(9)), ErrorCode::StorageBudgetExceeded);
    BOOST_CHECK(!tx->commit());
    tx = f.write();
    BOOST_REQUIRE(tx->quotas().reserve(SpaceQuota{ id<SpaceId>() }, amount(100000, 9999)));
    BOOST_REQUIRE(tx->commit());
    next = take(other->beginTransferWrite());
    errorIs(next->quotas().reserve(SpaceQuota{ id<SpaceId>() }, amount(0, 1)), ErrorCode::QuotaExceeded);
    BOOST_CHECK(!next->commit());
}
BOOST_AUTO_TEST_CASE(unknown_commit_never_permits_inverse_writes_and_fresh_store_reads_evidence) {
    Fixture f;
    auto raw = f.raw();
    bool active = false;
    {
        SqliteMetadataTransaction tx(*raw, active, { 8192, 1, 1 });
        BOOST_REQUIRE(tx.begin());
        BOOST_REQUIRE(tx.uploads().insert(upload()));
        BOOST_REQUIRE(tx.storage().reserve(bytes(16)));
        BOOST_REQUIRE(tx.quotas().reserve(SpaceQuota{ id<SpaceId>() }, amount(16, 1)));
        BOOST_REQUIRE(tx.transferAudit().append({ TransferEventKind::Create, id<RequestId>(), id<UserId>(), id<UploadId>(), now, {} }));
        auto deny = [](void*, int operation, const char* first, const char*, const char*, const char*) {
            return operation == SQLITE_TRANSACTION && first && std::string_view(first) != "BEGIN" ? SQLITE_DENY : SQLITE_OK;
        };
        BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(raw->handle(), deny, nullptr), SQLITE_OK);
        auto result = tx.commit();
        BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(raw->handle(), nullptr, nullptr), SQLITE_OK);
        errorIs(result, ErrorCode::CommitOutcomeUnknown);
        BOOST_CHECK(!raw->usable() && !active);
        errorIs(tx.storage().releaseReservation(bytes(16)), ErrorCode::InvalidArgument);
        errorIs(tx.uploads().find(id<UploadId>()), ErrorCode::InvalidArgument);
    }
    raw.reset();
    auto fresh = take(f.backend.connect());
    auto read = take(fresh->beginTransferRead());
    BOOST_CHECK(!take(read->uploads().find(id<UploadId>())));
    BOOST_CHECK_EQUAL(take(read->storage().get()).reserved.value(), 0);
    BOOST_CHECK_EQUAL(take(read->quotas().get(SpaceQuota{ id<SpaceId>() })).state.reserved.nodes.value(), 0);
    BOOST_CHECK(take(f.backend.readTransferAudit()).empty());
}

}  // namespace anof::infrastructure::sqlite::testing
