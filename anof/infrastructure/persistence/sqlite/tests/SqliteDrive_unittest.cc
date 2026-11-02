#define BOOST_TEST_MODULE SqliteDrive
#include <anof/infrastructure/crypto/SodiumTransferIdGenerator.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestDriveBackend.h>

#include <boost/test/unit_test.hpp>
using namespace anof;
using namespace anof::infrastructure::sqlite;
namespace {
template <class T>
T id(unsigned n) {
    char text[37];
    snprintf(text, sizeof(text), "00000000-0000-4000-8000-%012x", n);
    return T::parse(text).value();
}
ByteCount bytes(std::int64_t n) { return ByteCount::from(n).value(); }
NodeCount count(std::int64_t n) { return NodeCount::from(n).value(); }
UtcTime time(std::int64_t n) { return UtcTime{ std::chrono::microseconds{ n } }; }
anof::testing::DriveSeed seed() {
    anof::testing::DriveSeed s;
    auto& m = s.transfer.metadata;
    m.users.push_back({ { id<UserId>(1), UserRole::Administrator, {} }, true });
    m.users.push_back({ { id<UserId>(2), UserRole::Member, {} }, true });
    m.spaces.push_back({ id<SpaceId>(1), SpaceKind::Public, {}, id<NodeId>(1) });
    m.nodes.push_back({ { id<NodeId>(1), id<SpaceId>(1), {}, "", id<UserId>(1), {}, time(100), time(100) }, {}, false });
    m.quotas.push_back({ SpaceQuota{ id<SpaceId>(1) }, { { bytes(10000), count(100000) }, { bytes(0), count(1) }, {} } });
    s.transfer.storage = { bytes(10000), {}, {}, {} };
    s.nodeAccounting = { count(1), {}, {} };
    return s;
}
void directory(anof::testing::DriveSeed& s, unsigned n, unsigned parent, std::string name, unsigned owner = 1) {
    s.transfer.metadata.nodes.push_back({ { id<NodeId>(n), id<SpaceId>(1), id<NodeId>(parent), std::move(name), id<UserId>(owner), {}, time(100), time(100) }, {}, false });
    s.nodeAccounting.used = count(static_cast<std::int64_t>(s.transfer.metadata.nodes.size()));
    s.transfer.metadata.quotas[0].state.used.nodes = s.nodeAccounting.used;
}
void file(anof::testing::DriveSeed& s, unsigned n, unsigned parent, std::string name, unsigned blob = 1) {
    directory(s, n, parent, std::move(name));
    s.transfer.metadata.nodes.back().blobId = id<BlobId>(blob);
}
void blob(anof::testing::DriveSeed& s, unsigned n, std::int64_t length, std::optional<UtcTime> unref = {}) {
    auto hash = Sha256::parse(std::string(64, 'a')).value();
    s.blobRecords.push_back({ { id<BlobId>(n), FileLength::from(length).value(), hash }, BlobState::Available, time(100), time(100), {}, unref, {}, bytes(length), {} });
    s.transfer.metadata.blobs.push_back({ id<BlobId>(n), bytes(length), hash.bytes() });
    s.transfer.storage.retained = s.transfer.storage.retained.add(length).value();
}
struct Fixture {
    anof::infrastructure::sqlite::testing::SqliteTestDriveBackend backend;
    std::unique_ptr<DriveMetadataStore> store;
    explicit Fixture(const anof::testing::DriveSeed& s) {
        auto r = backend.reset(s);
        BOOST_REQUIRE(r);
        reopen();
    }
    void reopen() {
        store.reset();
        auto s = backend.connect();
        BOOST_REQUIRE(s);
        store = std::move(s).value();
    }
    std::unique_ptr<DriveTransaction> write() {
        auto tx = store->beginDriveWrite();
        BOOST_REQUIRE(tx);
        return std::move(tx).value();
    }
    void sql(const std::string& text) {
        auto c = SqliteConnection::open(backend.databasePath(), { 0 }, SqliteOpenMode::Inspect);
        BOOST_REQUIRE(c);
        BOOST_REQUIRE(c.value()->execute(text));
    }
};
TrashEntry entry(const TreeSnapshot& tree, unsigned n, UtcTime observed) {
    auto summary = summarizeTree(tree).value();
    auto it = std::find_if(tree.members.begin(), tree.members.end(), [&](const auto& m) { return m.node.metadata.id == tree.root; });
    auto& root = it->node.metadata;
    auto at = std::max(observed, summary.latestEvent);
    return { id<TrashId>(n), root.spaceId, root.id, *root.parentId, root.name, std::holds_alternative<DirectoryContent>(it->node.content) ? NodeKind::Directory : NodeKind::File, id<UserId>(1), at, trashExpiry(at).value(), {}, summary.total.nodes, summary.total.bytes, summary.soleCreator };
}
void trash(Fixture& f, unsigned node, unsigned tid) {
    auto tx = f.write();
    auto tree = tx->tree().activeSubtree(id<NodeId>(node), {});
    BOOST_REQUIRE(tree);
    auto e = entry(tree.value(), tid, time(50));
    BOOST_REQUIRE(tx->trash().insert(e));
    BOOST_REQUIRE(tx->tree().detach(tree.value(), { id<NodeId>(node), {} }, e));
    BOOST_REQUIRE(tx->commit());
}
}  // namespace
BOOST_AUTO_TEST_CASE(new_blob_with_unreferenced_time_fails_transaction_and_reopen) {
    Fixture f(seed());
    const BlobRecord record{ { id<BlobId>(1), FileLength::from(7).value(), Sha256::parse(std::string(64, 'a')).value() },
                             BlobState::Available,
                             time(100),
                             time(100),
                             {},
                             time(100),
                             {},
                             bytes(7),
                             {} };
    {
        auto tx = f.write();
        BOOST_REQUIRE(tx->nodeAccounting().reserve(count(1)));
        const auto inserted = tx->blobs().insert(record);
        BOOST_REQUIRE(!inserted);
        BOOST_CHECK(inserted.error().code == ErrorCode::IntegrityError);
        BOOST_CHECK(!tx->commit());
    }
    f.reopen();
    {
        auto tx = f.write();
        const auto stored = tx->blobs().record(record.content.id);
        BOOST_REQUIRE(stored);
        BOOST_CHECK(!stored.value());
        const auto accounting = tx->nodeAccounting().get();
        BOOST_REQUIRE(accounting);
        BOOST_CHECK_EQUAL(accounting.value().used.value(), 1);
        BOOST_CHECK_EQUAL(accounting.value().reserved.value(), 0);
        BOOST_CHECK_EQUAL(accounting.value().revision.value(), 1);
        BOOST_REQUIRE(tx->commit());
    }
}
BOOST_AUTO_TEST_CASE(tree_move_noop_conditions_and_revisions) {
    auto s = seed();
    directory(s, 2, 1, "a");
    directory(s, 3, 2, "child");
    directory(s, 4, 1, "dest");
    Fixture f(s);
    {
        auto tx = f.write();
        auto tree = tx->tree().activeSubtree(id<NodeId>(1), {}).value();
        auto changed = tx->tree().move({ tree, { id<NodeId>(1), {} }, id<NodeId>(4), "root", time(200) });
        BOOST_REQUIRE(!changed);
        BOOST_CHECK(changed.error().code == ErrorCode::Forbidden);
    }

    {
        auto tx = f.write();
        auto old = tx->tree().activeSubtree(id<NodeId>(2), {}).value();
        auto no = tx->tree().move({ old, { id<NodeId>(2), {} }, id<NodeId>(1), "a", time(200) });
        BOOST_REQUIRE(no);
        BOOST_CHECK(no.value().metadata.updatedAt == time(100));
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto tx = f.write();
        auto old = tx->tree().activeSubtree(id<NodeId>(2), {}).value();
        auto moved = tx->tree().move({ old, { id<NodeId>(2), {} }, id<NodeId>(4), "moved", time(50) });
        BOOST_REQUIRE(moved);
        BOOST_CHECK_EQUAL(moved.value().metadata.revision.value(), 2);
        BOOST_CHECK(moved.value().metadata.updatedAt == time(100));
        BOOST_CHECK_EQUAL(tx->nodes().find(id<NodeId>(3)).value()->metadata.revision.value(), 1);
        BOOST_CHECK_EQUAL(tx->nodes().find(id<NodeId>(1)).value()->metadata.revision.value(), 2);
        BOOST_CHECK_EQUAL(tx->nodes().find(id<NodeId>(4)).value()->metadata.revision.value(), 2);
        BOOST_REQUIRE(tx->commit());
    }
    f.reopen();
    {
        auto tx = f.write();
        auto old = tx->tree().activeSubtree(id<NodeId>(2), {}).value();
        BOOST_CHECK(!tx->tree().move({ old, { id<NodeId>(2), {} }, id<NodeId>(1), "stale", time(300) }));
        BOOST_CHECK(!tx->commit());
    }
    {
        auto tx = f.write();
        auto old = tx->tree().activeSubtree(id<NodeId>(2), {}).value();
        BOOST_CHECK(!tx->tree().move({ old, { id<NodeId>(2), Revision::from(2).value() }, id<NodeId>(3), "cycle", time(300) }));
    }
    {
        auto tx = f.write();
        auto old = tx->tree().activeSubtree(id<NodeId>(2), {}).value();
        old.members.pop_back();
        BOOST_CHECK(!tx->tree().move({ old, { id<NodeId>(2), Revision::from(2).value() }, id<NodeId>(1), "partial", time(300) }));
    }
}
BOOST_AUTO_TEST_CASE(trash_commit_membership_visibility_restore_and_audit_rollback) {
    auto s = seed();
    directory(s, 2, 1, "tree");
    directory(s, 3, 2, "child", 2);
    Fixture f(s);
    {
        auto tx = f.write();
        auto tree = tx->tree().activeSubtree(id<NodeId>(2), {}).value();
        auto e = entry(tree, 1, time(100));
        BOOST_REQUIRE(tx->trash().insert(e));
        BOOST_CHECK(!tx->commit());
    }
    trash(f, 2, 1);
    f.reopen();
    {
        auto tx = f.write();
        BOOST_CHECK(!tx->nodes().find(id<NodeId>(2)).value());
        BOOST_CHECK(!tx->nodes().find(id<NodeId>(3)).value());
        auto hidden = tx->trash().list({ { id<UserId>(1), UserRole::Member, {} }, id<SpaceId>(1), {}, {}, {} });
        BOOST_REQUIRE(hidden);
        BOOST_CHECK(hidden.value().items.empty());
        auto all = tx->trash().list({ { id<UserId>(1), UserRole::Administrator, {} }, id<SpaceId>(1), id<NodeId>(2), {}, {} });
        BOOST_REQUIRE(all);
        BOOST_CHECK_EQUAL(all.value().items.size(), 1);
    }
    f.sql("CREATE TRIGGER reject_drive_audit BEFORE INSERT ON drive_audit_events BEGIN SELECT RAISE(ABORT,'test'); END");
    {
        auto tx = f.write();
        auto tree = tx->tree().trashSubtree(id<TrashId>(1), {}).value();
        BOOST_REQUIRE(tx->tree().restore({ tree, { id<NodeId>(2), Revision::from(2).value() }, id<NodeId>(1), "restored", time(50) }, { id<TrashId>(1), {} }));
        BOOST_REQUIRE(tx->trash().erase({ id<TrashId>(1), {} }));
        BOOST_CHECK(!tx->driveAudit().append({ id<RequestId>(1), time(50), id<UserId>(1), TreeRestored{ id<TrashId>(1), id<NodeId>(2) } }));
        BOOST_CHECK(!tx->commit());
    }
    f.sql("DROP TRIGGER reject_drive_audit");
    f.reopen();
    {
        auto tx = f.write();
        auto tree = tx->tree().trashSubtree(id<TrashId>(1), {}).value();
        auto restored = tx->tree().restore({ tree, { id<NodeId>(2), Revision::from(2).value() }, id<NodeId>(1), "restored", time(50) }, { id<TrashId>(1), {} });
        BOOST_REQUIRE(restored);
        BOOST_CHECK(restored.value().metadata.updatedAt == time(100));
        BOOST_REQUIRE(tx->trash().erase({ id<TrashId>(1), {} }));
        BOOST_REQUIRE(tx->driveAudit().append({ id<RequestId>(1), time(50), id<UserId>(1), TreeRestored{ id<TrashId>(1), id<NodeId>(2) } }));
        BOOST_REQUIRE(tx->commit());
    }
    auto audit = f.backend.readDriveAudit();
    BOOST_REQUIRE(audit);
    BOOST_REQUIRE_EQUAL(audit.value().size(), 1);
    BOOST_CHECK(audit.value()[0].observedAt == time(50));
}
BOOST_AUTO_TEST_CASE(search_literal_binary_seek_budgets_and_handler_cleanup) {
    auto s = seed();
    directory(s, 2, 1, "a%_x");
    directory(s, 3, 1, "A%_x");
    directory(s, 4, 1, "目录%_x");
    Fixture f(s);
    Principal p{ id<UserId>(1), UserRole::Member, {} };
    {
        auto tx = f.write();
        auto a = tx->tree().search(p, { id<SpaceId>(1), SearchText::parse("%_").value(), PageSize::from(1).value(), {}, {} });
        BOOST_REQUIRE(a);
        BOOST_REQUIRE(a.value().next);
        BOOST_CHECK_EQUAL(a.value().items[0].metadata.name, "A%_x");
        auto b = tx->tree().search(p, { id<SpaceId>(1), SearchText::parse("%_").value(), PageSize::from(200).value(), a.value().next, {} });
        BOOST_REQUIRE(b);
        BOOST_CHECK_EQUAL(b.value().items.size(), 2);
        auto c = tx->tree().search(p, { id<SpaceId>(1), SearchText::parse("A").value(), {}, {}, {} });
        BOOST_REQUIRE(c);
        BOOST_CHECK_EQUAL(c.value().items.size(), 1);
    }
    {
        auto tx = f.write();
        QueryBudget budget;
        budget.candidates = 1;
        auto r = tx->tree().search(p, { id<SpaceId>(1), SearchText::parse("absent").value(), {}, {}, budget });
        BOOST_REQUIRE(!r);
        BOOST_CHECK(r.error().code == ErrorCode::QueryLimitExceeded);
        BOOST_CHECK(!tx->commit());
    }
    {
        auto tx = f.write();
        auto r = tx->tree().search(p, { id<SpaceId>(1), SearchText::parse("目录").value(), {}, {}, {} });
        BOOST_REQUIRE(r);
        BOOST_CHECK_EQUAL(r.value().items.size(), 1);
        BOOST_REQUIRE(tx->commit());
    }
}
BOOST_AUTO_TEST_CASE(tree_1000_1001_and_node_limit_are_bounded) {
    for (unsigned size : { 1000u, 1001u }) {
        auto s = seed();
        directory(s, 2, 1, "tree");
        for (unsigned n = 3; n < size + 2; ++n) directory(s, n, 2, "n" + std::to_string(n));
        Fixture f(s);
        auto tx = f.write();
        auto tree = tx->tree().activeSubtree(id<NodeId>(2), {});
        BOOST_REQUIRE(tree);
        BOOST_CHECK_EQUAL(tree.value().members.size(), size);
        auto moved = tx->tree().move({ tree.value(), { id<NodeId>(2), {} }, id<NodeId>(1), "renamed", time(100) });
        BOOST_CHECK(bool(moved) == (size == 1000));
    }
    Fixture f(seed());
    f.sql("UPDATE node_accounting SET used_nodes=99999");
    {
        auto tx = f.write();
        BOOST_REQUIRE(tx->nodeAccounting().reserve(count(1)));
        BOOST_REQUIRE(tx->nodeAccounting().commitReservation(count(1)));
        BOOST_CHECK_EQUAL(tx->nodeAccounting().get().value().used.value(), 100000);
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto tx = f.write();
        auto r = tx->nodeAccounting().chargeUsed(count(1));
        BOOST_REQUIRE(!r);
        BOOST_CHECK(r.error().code == ErrorCode::NodeLimitReached);
    }
    f.sql("UPDATE node_accounting SET revision=9223372036854775807");
    {
        auto tx = f.write();
        BOOST_CHECK(!tx->nodeAccounting().releaseUsed(count(1)));
    }
}
BOOST_AUTO_TEST_CASE(shared_blob_purge_logical_release_and_gc_zero_bytes) {
    auto s = seed();
    blob(s, 1, 7);
    blob(s, 2, 0, time(100));
    file(s, 2, 1, "one");
    file(s, 3, 1, "copy");
    s.transfer.metadata.quotas[0].state.used.bytes = bytes(14);
    Fixture f(s);
    trash(f, 2, 1);
    {
        auto tx = f.write();
        auto tree = tx->tree().trashSubtree(id<TrashId>(1), {}).value();
        BOOST_REQUIRE(tx->tree().eraseMembers({ id<TrashId>(1), {} }, tree));
        BOOST_REQUIRE(tx->trash().erase({ id<TrashId>(1), {} }));
        BOOST_REQUIRE(tx->quotas().releaseUsed(SpaceQuota{ id<SpaceId>(1) }, { bytes(7), count(1) }));
        BOOST_REQUIRE(tx->nodeAccounting().releaseUsed(count(1)));
        BOOST_REQUIRE(tx->blobs().hasNodeReference(id<BlobId>(1)).value());
        BOOST_REQUIRE(tx->commit());
    }
    f.reopen();
    {
        auto tx = f.write();
        BOOST_CHECK(!tx->blobs().markUnreferenced(id<BlobId>(1), {}, time(200)));
    }
    trash(f, 3, 2);
    {
        auto tx = f.write();
        auto tree = tx->tree().trashSubtree(id<TrashId>(2), {}).value();
        BOOST_REQUIRE(tx->tree().eraseMembers({ id<TrashId>(2), {} }, tree));
        BOOST_REQUIRE(tx->trash().erase({ id<TrashId>(2), {} }));
        BOOST_REQUIRE(tx->quotas().releaseUsed(SpaceQuota{ id<SpaceId>(1) }, { bytes(7), count(1) }));
        BOOST_REQUIRE(tx->nodeAccounting().releaseUsed(count(1)));
        auto b = tx->blobs().markUnreferenced(id<BlobId>(1), {}, time(50));
        BOOST_REQUIRE(b);
        BOOST_CHECK(*b.value().unreferencedAt == time(100));
        auto again = tx->blobs().markUnreferenced(id<BlobId>(1), b.value().revision, time(300));
        BOOST_REQUIRE(again);
        BOOST_CHECK(again.value().revision == b.value().revision);
        BOOST_REQUIRE(tx->commit());
    }
    f.reopen();
    {
        auto tx = f.write();
        auto stats = tx->blobs().gcStatistics();
        BOOST_REQUIRE(stats);
        BOOST_CHECK_EQUAL(stats.value().pendingBytes.value(), 7);
        BOOST_CHECK_EQUAL(stats.value().pendingObjects.value(), 2);
        auto early = tx->blobs().gcCandidates(time(50), PageSize::from(16).value(), {});
        BOOST_REQUIRE(early);
        BOOST_CHECK(early.value().items.empty());
    }
    {
        auto tx = f.write();
        auto marked = tx->blobs().markDeleting(id<BlobId>(2), {}, blobGcDeadline(time(100)).value());
        BOOST_REQUIRE(marked);
        BOOST_REQUIRE(tx->commit());
    }
    f.reopen();
    {
        auto tx = f.write();
        auto b = tx->blobs().record(id<BlobId>(2)).value().value();
        auto before = tx->storage().get().value();
        BOOST_REQUIRE(tx->blobs().eraseDeleting(id<BlobId>(2), b.revision));
        BOOST_REQUIRE(tx->storage().releaseRetained(bytes(0)));
        BOOST_CHECK(tx->storage().get().value().revision == before.revision);
        BOOST_REQUIRE(tx->commit());
    }
}
BOOST_AUTO_TEST_CASE(drive_ids_are_typed_random_and_unique) {
    auto ids = anof::infrastructure::crypto::makeSodiumDriveIdGenerator();
    BOOST_REQUIRE(ids);
    auto a = ids.value()->trashId();
    auto b = ids.value()->trashId();
    BOOST_REQUIRE(a && b);
    BOOST_CHECK(a.value() != b.value());
    BOOST_CHECK_EQUAL(a.value().toString()[14], '4');
}
BOOST_AUTO_TEST_CASE(completed_upload_deletion_evidence_is_once_and_survives_reopen) {
    auto s = seed();
    blob(s, 1, 7);
    file(s, 2, 1, "result");
    s.transfer.metadata.quotas[0].state.used.bytes = bytes(7);
    UploadTarget target{ id<NodeId>(1), "result" };
    auto digest = s.blobRecords[0].content.sha256;
    Upload u{ id<UploadId>(1), id<UserId>(1), id<OperationId>(1), { id<SpaceId>(1), target, FileLength::from(7).value(), digest }, target, UploadOffset::from(7).value(), UploadState::Completed, {}, digest, id<BlobId>(1), StagingLocation::Object, id<NodeId>(2), time(100), time(100), uploadExpiry(time(100), time(100)).value(), time(100), {} };
    s.transfer.uploads.push_back(u);
    Fixture f(s);
    trash(f, 2, 1);
    {
        auto tx = f.write();
        auto tree = tx->tree().trashSubtree(id<TrashId>(1), {}).value();
        auto marked = tx->uploads().markResultsDeleted({ id<NodeId>(2), id<NodeId>(2) }, time(50));
        BOOST_REQUIRE(marked);
        BOOST_CHECK_EQUAL(marked.value().value(), 1);
        BOOST_CHECK_EQUAL(tx->uploads().markResultsDeleted({ id<NodeId>(2) }, time(400)).value().value(), 0);
        BOOST_REQUIRE(tx->tree().eraseMembers({ id<TrashId>(1), {} }, tree));
        BOOST_REQUIRE(tx->trash().erase({ id<TrashId>(1), {} }));
        BOOST_REQUIRE(tx->quotas().releaseUsed(SpaceQuota{ id<SpaceId>(1) }, { bytes(7), count(1) }));
        BOOST_REQUIRE(tx->nodeAccounting().releaseUsed(count(1)));
        BOOST_REQUIRE(tx->blobs().markUnreferenced(id<BlobId>(1), {}, time(100)));
        BOOST_REQUIRE(tx->commit());
    }
    f.reopen();
    {
        auto tx = f.write();
        auto history = tx->uploads().find(id<UploadId>(1));
        BOOST_REQUIRE(history && history.value());
        BOOST_CHECK(history.value()->resultDeletedAt == time(100));
        BOOST_CHECK(history.value()->resultNodeId == id<NodeId>(2));
        BOOST_CHECK_EQUAL(history.value()->generation.value(), 2);
        BOOST_CHECK(!tx->blobs().hasUploadReference(id<BlobId>(1)).value());
        BOOST_CHECK(!tx->nodes().find(id<NodeId>(2)).value());
        BOOST_REQUIRE(tx->commit());
    }
}
BOOST_AUTO_TEST_CASE(deleting_rejects_new_references_and_overflow_rolls_back) {
    auto s = seed();
    blob(s, 1, 7, time(100));
    Fixture f(s);
    {
        auto tx = f.write();
        auto marked = tx->blobs().markDeleting(id<BlobId>(1), {}, blobGcDeadline(time(100)).value());
        BOOST_REQUIRE(marked);
        BOOST_REQUIRE(tx->commit());
    }
    f.reopen();
    {
        auto tx = f.write();
        BOOST_CHECK(!tx->nodes().insertFile({ { id<NodeId>(2), id<SpaceId>(1), id<NodeId>(1), "bad", id<UserId>(1), time(100) }, id<BlobId>(1) }));
        BOOST_CHECK(!tx->commit());
    }
    {
        auto tx = f.write();
        BOOST_CHECK_EQUAL(tx->blobs().gcStatistics().value().deletingObjects.value(), 1);
        BOOST_CHECK(!tx->storage().releaseRetained(bytes(8)));
        BOOST_CHECK(!tx->commit());
    }
    {
        auto tx = f.write();
        BOOST_CHECK_EQUAL(tx->storage().get().value().retained.value(), 7);
    }
}
BOOST_AUTO_TEST_CASE(gc_pages_and_trash_expiry_use_raw_time_and_seek) {
    auto s = seed();
    directory(s, 2, 1, "a");
    directory(s, 3, 1, "b");
    blob(s, 1, 0, time(100));
    blob(s, 2, 0, time(100));
    Fixture f(s);
    trash(f, 2, 1);
    trash(f, 3, 2);
    auto tx = f.write();
    auto page = tx->blobs().gcCandidates(blobGcDeadline(time(100)).value(), PageSize::from(1).value(), {});
    BOOST_REQUIRE(page);
    BOOST_REQUIRE(page.value().next);
    auto next = tx->blobs().gcCandidates(blobGcDeadline(time(100)).value(), PageSize::from(1).value(), page.value().next);
    BOOST_REQUIRE(next);
    BOOST_CHECK_EQUAL(next.value().items.size(), 1);
    BOOST_CHECK(!next.value().next);
    auto early = tx->trash().expired(time(50), PageSize::from(16).value(), {});
    BOOST_REQUIRE(early);
    BOOST_CHECK(early.value().items.empty());
    auto due = tx->trash().expired(trashExpiry(time(100)).value(), PageSize::from(1).value(), {});
    BOOST_REQUIRE(due);
    BOOST_REQUIRE(due.value().next);
    auto rest = tx->trash().expired(trashExpiry(time(100)).value(), PageSize::from(16).value(), due.value().next);
    BOOST_REQUIRE(rest);
    BOOST_CHECK_EQUAL(rest.value().items.size(), 1);
}
BOOST_AUTO_TEST_CASE(tree_write_conflicts_overflow_and_query_vm_budget_roll_back) {
    auto s = seed();
    directory(s, 2, 1, "a");
    directory(s, 3, 1, "b");
    Fixture f(s);
    {
        auto tx = f.write();
        auto tree = tx->tree().activeSubtree(id<NodeId>(2), {}).value();
        auto r = tx->tree().move({ tree, { id<NodeId>(2), {} }, id<NodeId>(1), "b", time(100) });
        BOOST_REQUIRE(!r);
        BOOST_CHECK(r.error().code == ErrorCode::NameConflict);
        BOOST_CHECK(!tx->commit());
    }
    f.sql("UPDATE nodes SET revision=9223372036854775807 WHERE name=''");
    {
        auto tx = f.write();
        auto tree = tx->tree().activeSubtree(id<NodeId>(2), {}).value();
        BOOST_CHECK(!tx->tree().move({ tree, { id<NodeId>(2), {} }, id<NodeId>(1), "new", time(200) }));
        BOOST_CHECK(!tx->commit());
    }
    f.reopen();
    {
        auto tx = f.write();
        BOOST_CHECK_EQUAL(tx->nodes().find(id<NodeId>(2)).value()->metadata.name, "a");
    }
    auto many = seed();
    for (unsigned n = 2; n < 102; ++n) directory(many, n, 1, "node" + std::to_string(n));
    Fixture query(many);
    {
        auto tx = query.write();
        QueryBudget budget;
        budget.instructions = 1000;
        auto r = tx->tree().search({ id<UserId>(1), UserRole::Administrator, {} }, { id<SpaceId>(1), SearchText::parse("missing").value(), PageSize::from(200).value(), {}, budget });
        BOOST_REQUIRE(!r);
        BOOST_CHECK(r.error().code == ErrorCode::QueryLimitExceeded);
    }
    {
        auto tx = query.write();
        auto r = tx->tree().search({ id<UserId>(1), UserRole::Administrator, {} }, { id<SpaceId>(1), SearchText::parse("node").value(), PageSize::from(200).value(), {}, {} });
        BOOST_REQUIRE(r);
        BOOST_CHECK_EQUAL(r.value().items.size(), 100);
    }
}
BOOST_AUTO_TEST_CASE(seed_rejects_inconsistent_membership_blob_and_ledger) {
    anof::infrastructure::sqlite::testing::SqliteTestDriveBackend backend;
    auto s = seed();
    s.nodeAccounting.used = count(2);
    BOOST_CHECK(!backend.reset(s));
    s = seed();
    blob(s, 1, 0);
    BOOST_CHECK(!backend.reset(s));
    s = seed();
    blob(s, 1, 0, time(100));
    file(s, 2, 1, "referenced");
    BOOST_CHECK(!backend.reset(s));
}
BOOST_AUTO_TEST_CASE(all_drive_audit_variants_round_trip_without_live_node_foreign_keys) {
    Fixture f(seed());
    std::vector<DriveAction> actions{ NodeRenamed{ id<NodeId>(2) }, NodeMoved{ id<NodeId>(2), id<NodeId>(1), id<NodeId>(3) }, FileCopied{ id<NodeId>(2), id<NodeId>(3) }, TreeTrashed{ id<TrashId>(1), id<NodeId>(2) }, TreeRestored{ id<TrashId>(1), id<NodeId>(2) }, TreePurged{ id<TrashId>(1), id<NodeId>(2) }, GcMarked{ id<BlobId>(1) }, GcFinished{ id<BlobId>(1) } };
    auto tx = f.write();
    for (const auto& action : actions) BOOST_REQUIRE(tx->driveAudit().append({ id<RequestId>(1), time(50), action.index() >= 5 ? DriveActor(MaintenanceActor{}) : DriveActor(id<UserId>(1)), action }));
    BOOST_REQUIRE(tx->commit());
    tx.reset();
    f.reopen();
    auto events = f.backend.readDriveAudit();
    BOOST_REQUIRE(events);
    BOOST_REQUIRE_EQUAL(events.value().size(), actions.size());
    for (std::size_t i = 0; i < actions.size(); ++i) {
        BOOST_CHECK_EQUAL(events.value()[i].action.index(), i);
        BOOST_CHECK(events.value()[i].observedAt == time(50));
    }
}
