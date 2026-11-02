#define BOOST_TEST_MODULE DriveServiceIntegration
#include <anof/application/transfer/DownloadService.h>
#include <anof/application/transfer/TransferRecovery.h>
#include <tests/integration/DriveFixture.h>
#include <tests/integration/MaintenanceStateFixture.h>
using namespace anof;
using namespace anof::testing::drive;
namespace {
NodeCondition tag(const NodeDetails& n) { return { n.metadata.id, n.metadata.revision }; }
TrashCondition tag(const TrashView& t) { return { t.entry.id, t.entry.revision }; }
template <class T>
void error(Result<T> r, ErrorCode code) {
    BOOST_REQUIRE(!r);
    BOOST_CHECK(r.error().code == code);
}
}  // namespace
BOOST_AUTO_TEST_CASE(rename_move_noop_stale_cycle_and_private_target) {
    Fixture f;
    auto s = f.service();
    auto a = f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "a");
    auto b = f.mkdir(*f.admin, *f.personal, a.metadata.id, "b");
    a = f.node(a.metadata.id);
    error(s.execute(*f.admin, CopyFileCommand{ a.metadata.id, tag(a), f.personal->id, f.personal->rootNodeId, "directory-copy" }), ErrorCode::NotAFile);
    auto unchanged = take(s.execute(*f.admin, UpdateNodeCommand{ a.metadata.id, tag(a), {}, "a" }));
    BOOST_CHECK(unchanged.metadata.revision == a.metadata.revision);
    error(s.execute(*f.admin, UpdateNodeCommand{ a.metadata.id, tag(a), b.metadata.id, {} }), ErrorCode::DirectoryCycle);
    auto moved = take(s.execute(*f.admin, UpdateNodeCommand{ b.metadata.id, tag(b), f.personal->rootNodeId, "moved" }));
    BOOST_CHECK(moved.metadata.parentId == f.personal->rootNodeId);
    error(s.execute(*f.admin, UpdateNodeCommand{ b.metadata.id, tag(b), {}, "stale" }), ErrorCode::PreconditionFailed);
    error(s.execute(*f.admin, UpdateNodeCommand{ moved.metadata.id, tag(moved), f.common->rootNodeId, {} }), ErrorCode::CrossSpaceMove);
    auto member = f.member("alice");
    error(s.execute(member, UpdateNodeCommand{ moved.metadata.id, tag(moved), {}, "hidden" }), ErrorCode::NotFound);
    f.reopen();
    BOOST_CHECK_EQUAL(f.node(moved.metadata.id).metadata.name, "moved");
}
BOOST_AUTO_TEST_CASE(copy_trash_restore_purge_preserves_physical_and_upload_history) {
    Fixture f;
    auto s = f.service();
    auto u = f.publish(f.createUpload(f.personal->rootNodeId, "source", 7));
    auto source = f.node(*u.resultNodeId);
    auto copy = take(s.execute(*f.admin, CopyFileCommand{ source.metadata.id, tag(source), f.personal->id, f.personal->rootNodeId, "copy" }));
    auto trashed = take(s.execute(*f.admin, TrashNodeCommand{ source.metadata.id, tag(source) }));
    auto hidden = take(s.execute(*f.admin, SearchNodesCommand{ f.personal->id, SearchText::parse("source").value(), {}, {} }));
    BOOST_CHECK(hidden.items.empty());
    auto got = take(s.execute(*f.admin, GetTrashCommand{ trashed.entry.id }));
    BOOST_CHECK(got.entry.rootNodeId == source.metadata.id);
    auto restored = take(s.execute(*f.admin, RestoreTrashCommand{ trashed.entry.id, tag(trashed), {}, "again" }));
    trashed = take(s.execute(*f.admin, TrashNodeCommand{ restored.metadata.id, tag(restored) }));
    BOOST_REQUIRE(s.execute(*f.admin, PurgeTrashCommand{ trashed.entry.id, tag(trashed) }));
    auto history = take(f.uploads().get(*f.admin, u.id));
    BOOST_CHECK(history.resultDeletedAt.has_value());
    BOOST_CHECK(history.resultNodeId == source.metadata.id);
    auto replay = take(f.uploads().acceptComplete(*f.admin, { u.id, {} }));
    BOOST_CHECK(replay.resultDeletedAt == history.resultDeletedAt);
    {
        auto r = take(f.store().beginDriveRead());
        auto physical = take(r->storage().get());
        BOOST_CHECK_EQUAL(physical.retained.value(), 7);
        auto gc = take(r->blobLifecycle().gcStatistics());
        BOOST_CHECK_EQUAL(gc.pendingObjects.value(), 0);
        auto q = take(r->quotas().get(SpaceQuota{ f.personal->id }));
        BOOST_CHECK_EQUAL(q.state.used.bytes.value(), 7);
    }
    auto last = take(s.execute(*f.admin, TrashNodeCommand{ copy.metadata.id, tag(copy) }));
    BOOST_REQUIRE(s.execute(*f.admin, PurgeTrashCommand{ last.entry.id, tag(last) }));
    error(s.execute(*f.admin, PurgeTrashCommand{ last.entry.id, tag(last) }), ErrorCode::NotFound);
    f.reopen();
    auto r = take(f.store().beginDriveRead());
    auto gc = take(r->blobLifecycle().gcStatistics());
    BOOST_CHECK_EQUAL(gc.pendingBytes.value(), 7);
    BOOST_CHECK_EQUAL(gc.pendingObjects.value(), 1);
    BOOST_CHECK_EQUAL(take(r->nodeAccounting().get()).used.value(), 2);
}
BOOST_AUTO_TEST_CASE(public_mixed_ownership_is_atomic_and_trash_pages_are_private) {
    Fixture f;
    auto s = f.service();
    auto alice = f.member("alice");
    auto parent = f.mkdir(alice, *f.common, f.common->rootNodeId, "parent");
    auto child = f.mkdir(*f.admin, *f.common, parent.metadata.id, "child");
    parent = f.node(parent.metadata.id);
    error(s.execute(alice, TrashNodeCommand{ parent.metadata.id, tag(parent) }), ErrorCode::Forbidden);
    BOOST_CHECK_EQUAL(f.node(child.metadata.id).metadata.name, "child");
    auto t = take(s.execute(*f.admin, TrashNodeCommand{ parent.metadata.id, tag(parent) }));
    BOOST_CHECK(!t.entry.soleCreatorId);
    auto page = take(s.execute(alice, ListTrashCommand{ f.common->id, {}, {}, {} }));
    BOOST_CHECK(page.items.empty());
    error(s.execute(alice, GetTrashCommand{ t.entry.id }), ErrorCode::NotFound);
    BOOST_REQUIRE(s.execute(*f.admin, PurgeTrashCommand{ t.entry.id, tag(t) }));
    f.reopen();
    auto r = take(f.store().beginDriveRead());
    BOOST_CHECK_EQUAL(take(r->quotas().get(SpaceQuota{ f.common->id })).state.used.nodes.value(), 1);
    BOOST_CHECK_EQUAL(take(r->nodeAccounting().get()).used.value(), 3);
}
BOOST_AUTO_TEST_CASE(restore_missing_parent_conflict_and_seek_binding) {
    Fixture f;
    auto s = f.service();
    auto p = f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "parent");
    auto child = f.mkdir(*f.admin, *f.personal, p.metadata.id, "child");
    auto t = take(s.execute(*f.admin, TrashNodeCommand{ child.metadata.id, tag(child) }));
    p = f.node(p.metadata.id);
    auto pt = take(s.execute(*f.admin, TrashNodeCommand{ p.metadata.id, tag(p) }));
    error(s.execute(*f.admin, RestoreTrashCommand{ t.entry.id, tag(t), {}, {} }), ErrorCode::RestoreTargetUnavailable);
    f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "child");
    error(s.execute(*f.admin, RestoreTrashCommand{ t.entry.id, tag(t), f.personal->rootNodeId, {} }), ErrorCode::NameConflict);
    auto list = take(s.execute(*f.admin, ListTrashCommand{ f.personal->id, {}, PageSize::from(1).value(), {} }));
    BOOST_REQUIRE(list.next);
    error(s.execute(*f.admin, ListTrashCommand{ f.common->id, {}, PageSize::from(1).value(), list.next }), ErrorCode::InvalidArgument);
    auto restored = take(s.execute(*f.admin, RestoreTrashCommand{ t.entry.id, tag(t), f.personal->rootNodeId, "renamed" }));
    BOOST_CHECK_EQUAL(restored.metadata.name, "renamed");
    BOOST_REQUIRE(s.execute(*f.admin, PurgeTrashCommand{ pt.entry.id, tag(pt) }));
}
BOOST_AUTO_TEST_CASE(upload_receiving_survives_hidden_parent_and_ready_target_can_change) {
    Fixture f;
    auto s = f.service();
    auto parent = f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "destination");
    auto u = f.createUpload(parent.metadata.id, "pending", 7);
    auto t = take(s.execute(*f.admin, TrashNodeCommand{ parent.metadata.id, tag(parent) }));
    auto patch = take(f.uploads().preparePatch(*f.admin, { u.id, {}, ByteCount::from(7).value() }));
    u = take(f.uploads().commitPatch(*f.admin, patch));
    error(f.uploads().acceptComplete(*f.admin, { u.id, {} }), ErrorCode::NotFound);
    u = take(f.uploads().acceptComplete(*f.admin, { u.id, UploadTarget{ f.personal->rootNodeId, "new" } }));
    u = take(f.uploads().recordVerification(u.id, u.generation, Sha256::parse(std::string(64, 'a')).value(), request()));
    auto root = f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "second");
    u = take(f.uploads().acceptComplete(*f.admin, { u.id, UploadTarget{ root.metadata.id, "new" } }));
    take(s.execute(*f.admin, TrashNodeCommand{ root.metadata.id, tag(root) }));
    error(f.uploads().preparePublish(u.id, u.generation), ErrorCode::NotFound);
    auto ready = take(f.uploads().read(u.id));
    BOOST_CHECK(ready.state == UploadState::Ready);
    BOOST_REQUIRE(ready.lastError);
    BOOST_CHECK(*ready.lastError == TransferFailure::NotFound);
    u = take(f.uploads().acceptComplete(*f.admin, { u.id, UploadTarget{ f.personal->rootNodeId, "final" } }));
    u = take(f.uploads().preparePublish(u.id, u.generation));
    u = take(f.uploads().commitPublish(u.id, u.generation, request()));
    f.reopen();
    auto r = take(f.store().beginDriveRead());
    BOOST_CHECK_EQUAL(take(r->nodeAccounting().get()).reserved.value(), 0);
}
BOOST_AUTO_TEST_CASE(upload_cancel_failure_and_expiry_release_node_reservation_once) {
    Fixture f;
    for (auto state : { UploadState::Cancelled, UploadState::Failed, UploadState::Expired }) {
        auto u = f.createUpload(f.personal->rootNodeId, "unused");
        u = take(f.uploads().terminate(u.id, u.generation, state, TransferFailure::InternalError, request()));
        take(f.uploads().terminate(u.id, u.generation, state, TransferFailure::InternalError, request()));
        take(f.uploads().confirmCleanup(u.id, u.generation));
    }
    f.reopen();
    auto r = take(f.store().beginDriveRead());
    auto n = take(r->nodeAccounting().get());
    BOOST_CHECK_EQUAL(n.used.value(), 2);
    BOOST_CHECK_EQUAL(n.reserved.value(), 0);
}

BOOST_AUTO_TEST_CASE(copy_quota_failure_rolls_back_and_full_physical_budget_allows_sharing) {
    Fixture f;
    auto u = f.publish(f.createUpload(f.personal->rootNodeId, "source", 7));
    auto source = f.node(*u.resultNodeId);
    auto parent = f.node(f.personal->rootNodeId);
    {
        auto tx = take(f.store().beginDriveWrite());
        auto q = take(tx->quotas().get(SpaceQuota{ f.personal->id }));
        take(tx->quotas().setLimit(SpaceQuota{ f.personal->id }, q.revision, { ByteCount::from(7).value(), NodeCount::from(100).value() }));
        auto storage = take(tx->storage().get());
        take(tx->storage().setBudget(storage.revision, ByteCount::from(7).value()));
        BOOST_REQUIRE(tx->commit());
    }
    error(f.service().execute(*f.admin, CopyFileCommand{ source.metadata.id, tag(source), f.personal->id, f.personal->rootNodeId, "copy" }), ErrorCode::QuotaExceeded);
    BOOST_CHECK(f.node(f.personal->rootNodeId).metadata.revision == parent.metadata.revision);
    {
        auto tx = take(f.store().beginDriveWrite());
        BOOST_CHECK_EQUAL(take(tx->nodeAccounting().get()).used.value(), 3);
        auto q = take(tx->quotas().get(SpaceQuota{ f.personal->id }));
        take(tx->quotas().setLimit(SpaceQuota{ f.personal->id }, q.revision, { ByteCount::from(14).value(), NodeCount::from(100).value() }));
        BOOST_REQUIRE(tx->commit());
    }
    take(f.service().execute(*f.admin, CopyFileCommand{ source.metadata.id, tag(source), f.personal->id, f.personal->rootNodeId, "copy" }));
    f.reopen();
    auto r = take(f.store().beginDriveRead());
    BOOST_CHECK_EQUAL(take(r->storage().get()).retained.value(), 7);
    BOOST_CHECK_EQUAL(take(r->quotas().get(SpaceQuota{ f.personal->id })).state.used.bytes.value(), 14);
}

BOOST_AUTO_TEST_CASE(trash_restore_and_final_reference_time_survive_utc_rollback) {
    Fixture f;
    struct TestClock : Clock {
        UtcTime value;
        UtcTime now() const override { return value; }
    };
    auto clock = std::make_unique<TestClock>();
    clock->value = f.resources.clock->now();
    auto* observed = clock.get();
    f.resources.clock = std::move(clock);
    const auto start = observed->value;
    auto u = f.publish(f.createUpload(f.personal->rootNodeId, "file"));
    auto file = f.node(*u.resultNodeId);
    observed->value = start - std::chrono::seconds(2);
    auto t = take(f.service().execute(*f.admin, TrashNodeCommand{ file.metadata.id, tag(file) }));
    BOOST_CHECK(t.entry.deletedAt >= file.metadata.updatedAt);
    auto restored = take(f.service().execute(*f.admin, RestoreTrashCommand{ t.entry.id, tag(t), {}, {} }));
    BOOST_CHECK(restored.metadata.updatedAt >= t.entry.deletedAt);
    t = take(f.service().execute(*f.admin, TrashNodeCommand{ restored.metadata.id, tag(restored) }));
    BOOST_REQUIRE(f.service().execute(*f.admin, PurgeTrashCommand{ t.entry.id, tag(t) }));
    f.reopen();
    auto r = take(f.store().beginDriveRead());
    auto record = take(r->blobLifecycle().record(*u.pendingBlobId));
    BOOST_REQUIRE(record && record->unreferencedAt);
    BOOST_CHECK(*record->unreferencedAt >= t.entry.deletedAt);
    BOOST_CHECK(take(r->uploads().find(u.id))->resultDeletedAt >= t.entry.deletedAt);
}

namespace {
struct FaultStore : DriveMetadataStore {
    DriveMetadataStore& inner;
    bool auditFailure = false;
    unsigned commits = 0;
    explicit FaultStore(DriveMetadataStore& s) : inner(s) {}
    struct Transaction : DriveTransaction, DriveAuditRepository {
        std::unique_ptr<DriveTransaction> tx;
        FaultStore& owner;
        Transaction(std::unique_ptr<DriveTransaction> t, FaultStore& o) : tx(std::move(t)), owner(o) {}
        AuthorizationReader& authorization() override { return tx->authorization(); }
        UserRepository& users() override { return tx->users(); }
        SessionRepository& sessions() override { return tx->sessions(); }
        InviteRepository& invites() override { return tx->invites(); }
        SpaceRepository& spaces() override { return tx->spaces(); }
        FileNodeRepository& nodes() override { return tx->nodes(); }
        DriveQuotaRepository& quotas() override { return tx->quotas(); }
        SettingsRepository& settings() override { return tx->settings(); }
        AuditRepository& directoryAudit() override { return tx->directoryAudit(); }
        IdentityAuditRepository& identityAudit() override { return tx->identityAudit(); }
        DriveUploadRepository& uploads() override { return tx->uploads(); }
        LifecycleBlobRepository& blobs() override { return tx->blobs(); }
        DriveStorageRepository& storage() override { return tx->storage(); }
        TransferAuditRepository& transferAudit() override { return tx->transferAudit(); }
        TreeRepository& tree() override { return tx->tree(); }
        TrashRepository& trash() override { return tx->trash(); }
        NodeAccountingRepository& nodeAccounting() override { return tx->nodeAccounting(); }
        DriveAuditRepository& driveAudit() override { return *this; }
        Result<void> append(const DriveEvent& e) override {
            auto written = tx->driveAudit().append(e);
            if (!written) return written;
            return owner.auditFailure ? Result<void>{ ErrorCode::InternalError } : Result<void>{};
        }
        Result<void> commit() override {
            ++owner.commits;
            auto committed = tx->commit();
            return committed ? Result<void>{ ErrorCode::CommitOutcomeUnknown } : committed;
        }
    };
    Result<std::unique_ptr<DriveTransaction>> beginDriveWrite() override {
        auto tx = inner.beginDriveWrite();
        if (!tx) return tx.error();
        return std::unique_ptr<DriveTransaction>(new Transaction(std::move(tx).value(), *this));
    }
    Result<std::unique_ptr<DriveReadSnapshot>> beginDriveRead() override { return inner.beginDriveRead(); }
    Result<std::unique_ptr<TransferTransaction>> beginTransferWrite() override { return inner.beginTransferWrite(); }
    Result<std::unique_ptr<TransferReadSnapshot>> beginTransferRead() override { return inner.beginTransferRead(); }
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override { return inner.beginWrite(); }
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override { return inner.beginRead(); }
};
}  // namespace
BOOST_AUTO_TEST_CASE(audit_failure_rolls_back_but_uncertain_commit_is_not_retried) {
    Fixture f;
    auto u = f.publish(f.createUpload(f.personal->rootNodeId, "source", 7));
    auto file = f.node(*u.resultNodeId);
    auto t = take(f.service().execute(*f.admin, TrashNodeCommand{ file.metadata.id, tag(file) }));
    FaultStore faulty(f.store());
    DriveService s(faulty, *f.resources.clock, *f.resources.nodeIds, *f.resources.driveIds);
    faulty.auditFailure = true;
    error(s.execute(*f.admin, PurgeTrashCommand{ t.entry.id, tag(t) }), ErrorCode::InternalError);
    BOOST_CHECK_EQUAL(faulty.commits, 0);
    {
        auto r = take(f.store().beginDriveRead());
        BOOST_CHECK(take(r->trash().find(t.entry.id)).has_value());
        BOOST_CHECK(!take(r->uploads().find(u.id))->resultDeletedAt);
        BOOST_CHECK_EQUAL(take(r->quotas().get(SpaceQuota{ f.personal->id })).state.used.bytes.value(), 7);
        BOOST_CHECK_EQUAL(take(r->nodeAccounting().get()).used.value(), 3);
    }
    faulty.auditFailure = false;
    error(s.execute(*f.admin, PurgeTrashCommand{ t.entry.id, tag(t) }), ErrorCode::CommitOutcomeUnknown);
    BOOST_CHECK_EQUAL(faulty.commits, 1);
    auto r = take(f.store().beginDriveRead());
    BOOST_CHECK(!take(r->trash().find(t.entry.id)));
    BOOST_CHECK(take(r->uploads().find(u.id))->resultDeletedAt.has_value());
    BOOST_CHECK_EQUAL(take(r->nodeAccounting().get()).used.value(), 2);
    BOOST_CHECK_EQUAL(take(r->storage().get()).retained.value(), 7);
}

BOOST_AUTO_TEST_CASE(condition_kind_is_preserved_until_authorized_and_never_mutates) {
    Fixture f;
    auto s = f.service();
    auto member = f.member("alice");
    auto upload = f.publish(f.createUpload(f.personal->rootNodeId, "source"));
    auto node = f.node(*upload.resultNodeId);
    // UUID和revision完全相同，仅资源种类不同，不能降格为同类条件。
    const TrashCondition wrongNode{ TrashId::parse(node.metadata.id.toString()).value(), node.metadata.revision };
    const UpdateNodeCommand update{ node.metadata.id, wrongNode, {}, "changed" };
    const CopyFileCommand copy{ node.metadata.id, wrongNode, f.personal->id, f.personal->rootNodeId, "copy" };
    const TrashNodeCommand trash{ node.metadata.id, wrongNode };
    for (const auto* ctx : { &*f.admin, &member }) {
        auto code = ctx == &*f.admin ? ErrorCode::PreconditionFailed : ErrorCode::NotFound;
        error(s.execute(*ctx, update), code);
        error(s.execute(*ctx, copy), code);
        error(s.execute(*ctx, trash), code);
    }
    auto invalid = *f.admin;
    invalid.credential.digest = SessionDigest{ SessionDigest::Bytes{} };
    error(s.execute(invalid, update), ErrorCode::Unauthenticated);
    error(s.execute(invalid, copy), ErrorCode::Unauthenticated);
    error(s.execute(invalid, trash), ErrorCode::Unauthenticated);
    BOOST_CHECK(f.node(node.metadata.id).metadata.revision == node.metadata.revision);
    BOOST_CHECK_EQUAL(f.node(node.metadata.id).metadata.name, "source");
    auto entry = take(s.execute(*f.admin, TrashNodeCommand{ node.metadata.id, tag(node) }));
    const NodeCondition wrongTrash{ NodeId::parse(entry.entry.id.toString()).value(), entry.entry.revision };
    const RestoreTrashCommand restore{ entry.entry.id, wrongTrash, {}, {} };
    const PurgeTrashCommand purge{ entry.entry.id, wrongTrash };
    for (const auto* ctx : { &*f.admin, &member }) {
        auto code = ctx == &*f.admin ? ErrorCode::PreconditionFailed : ErrorCode::NotFound;
        error(s.execute(*ctx, restore), code);
        error(s.execute(*ctx, purge), code);
    }
    error(s.execute(invalid, restore), ErrorCode::Unauthenticated);
    error(s.execute(invalid, purge), ErrorCode::Unauthenticated);
    BOOST_REQUIRE(s.execute(*f.admin, GetTrashCommand{ entry.entry.id }));
    BOOST_CHECK(!take(f.uploads().get(*f.admin, upload.id)).resultDeletedAt);
    // 可见但无整树修改权限时，403优先于条件不匹配。
    auto shared = f.mkdir(*f.admin, *f.common, f.common->rootNodeId, "shared");
    const TrashCondition wrongShared{ TrashId::parse(shared.metadata.id.toString()).value(), shared.metadata.revision };
    error(s.execute(member, UpdateNodeCommand{ shared.metadata.id, wrongShared, {}, "changed" }), ErrorCode::Forbidden);
    error(s.execute(member, TrashNodeCommand{ shared.metadata.id, wrongShared }), ErrorCode::Forbidden);
    auto root = f.node(f.personal->rootNodeId);
    error(s.execute(*f.admin, TrashNodeCommand{ root.metadata.id, wrongNode }), ErrorCode::Forbidden);
    f.reopen();
    auto r = take(f.store().beginDriveRead());
    BOOST_CHECK(take(r->trash().find(entry.entry.id)).has_value());
    BOOST_CHECK(!take(r->uploads().find(upload.id))->resultDeletedAt);
}

BOOST_AUTO_TEST_CASE(zero_byte_cross_space_copy_charges_creator_node_without_physical_growth) {
    Fixture f;
    auto alice = f.member("alice");
    auto u = f.publish(f.createUpload(f.personal->rootNodeId, "empty"));
    auto source = f.node(*u.resultNodeId);
    auto shared = take(f.service().execute(*f.admin, CopyFileCommand{
                                                         source.metadata.id, tag(source), f.common->id, f.common->rootNodeId, "shared-empty" }));
    Space personal = *f.personal;
    std::optional<UserId> creator;
    {
        auto read = take(f.store().beginDriveRead());
        creator = take(read->authorization().requireActiveSession(alice.credential, f.resources.clock->now())).userId;
        personal = *take(read->spaces().findPrivate(*creator));
    }
    auto copy = take(f.service().execute(alice, CopyFileCommand{
                                                    shared.metadata.id, tag(shared), personal.id, personal.rootNodeId, "my-empty" }));
    BOOST_CHECK(copy.metadata.id != shared.metadata.id);
    BOOST_CHECK(copy.metadata.createdBy == *creator);
    f.reopen();
    auto read = take(f.store().beginDriveRead());
    BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 0);
    auto quota = take(read->quotas().get(SpaceQuota{ personal.id }));
    BOOST_CHECK_EQUAL(quota.state.used.bytes.value(), 0);
    BOOST_CHECK_EQUAL(quota.state.used.nodes.value(), 2);
    BOOST_CHECK_EQUAL(take(read->nodeAccounting().get()).used.value(), 6);
}

BOOST_AUTO_TEST_CASE(upload_parent_move_keeps_id_binding_and_publishes_at_current_location) {
    Fixture f;
    auto parent = f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "parent");
    auto destination = f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "destination");
    auto upload = f.createUpload(parent.metadata.id, "pending", 3);
    auto moved = take(f.service().execute(*f.admin, UpdateNodeCommand{
                                                        parent.metadata.id, tag(parent), destination.metadata.id, "moved" }));
    upload = f.publish(upload);
    auto file = f.node(*upload.resultNodeId);
    BOOST_CHECK(file.metadata.parentId == moved.metadata.id);
    BOOST_CHECK(f.node(moved.metadata.id).metadata.parentId == destination.metadata.id);
    f.reopen();
    auto read = take(f.store().beginDriveRead());
    BOOST_CHECK_EQUAL(take(read->nodeAccounting().get()).reserved.value(), 0);
    BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 3);
}

BOOST_AUTO_TEST_CASE(unavailable_nodes_keep_metadata_charge_and_reject_content_operations) {
    Fixture f;
    auto uploaded = f.publish(f.createUpload(f.personal->rootNodeId, "missing", 3));
    markMissing(f, *uploaded.pendingBlobId);
    auto node = f.node(*uploaded.resultNodeId);
    auto denied = prepareDownload(f.store(), *f.resources.clock, *f.admin, node.metadata.id);
    error(std::move(denied), ErrorCode::IntegrityError);
    error(f.service().execute(*f.admin, CopyFileCommand{ node.metadata.id, tag(node), f.personal->id, f.personal->rootNodeId, "copy" }), ErrorCode::IntegrityError);
    auto outsider = f.member("outsider");
    error(prepareDownload(f.store(), *f.resources.clock, outsider, node.metadata.id), ErrorCode::NotFound);
    auto storage = take(f.uploads().storage(*f.admin));
    BOOST_CHECK_EQUAL(storage.accounting.retained.value(), 3);
    BOOST_CHECK_EQUAL(storage.gc.unavailableBytes.value(), 3);
    BOOST_CHECK_EQUAL(storage.gc.unavailableObjects.value(), 1);
    BOOST_CHECK_EQUAL(storage.gc.pendingBytes.value(), 0);
    BOOST_REQUIRE(auditTransferMetadata(f.store(), f.resources.clock->now()));
    auto entry = take(f.service().execute(*f.admin, TrashNodeCommand{ node.metadata.id, tag(node) })).entry;
    BOOST_REQUIRE(auditTransferMetadata(f.store(), f.resources.clock->now()));
    BOOST_REQUIRE(f.service().execute(*f.admin, PurgeTrashCommand{ entry.id, TrashCondition{ entry.id, entry.revision } }));
    BOOST_REQUIRE(auditTransferMetadata(f.store(), f.resources.clock->now()));
    storage = take(f.uploads().storage(*f.admin));
    BOOST_CHECK_EQUAL(storage.accounting.retained.value(), 3);
    BOOST_CHECK_EQUAL(storage.gc.unavailableObjects.value(), 1);
    BOOST_CHECK_EQUAL(storage.gc.pendingObjects.value(), 0);
}
