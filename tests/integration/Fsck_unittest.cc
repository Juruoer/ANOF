#define BOOST_TEST_MODULE FsckIntegration
#include <anof/application/drive/MaintenanceService.h>
#include <anof/application/maintenance/Fsck.h>
#include <anof/infrastructure/crypto/SodiumMaintenanceDigest.h>
#include <anof/infrastructure/crypto/SodiumTransferIdGenerator.h>
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataInspection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataRepair.h>
#include <anof/infrastructure/storage/local/LocalMaintenanceFileStore.h>
#include <anof/infrastructure/system/LinuxOfflineInstanceAccess.h>
#include <anof/runtime/OfflineRecovery.h>
#include <sys/stat.h>
#include <tests/integration/DriveFixture.h>

#include <fstream>
using namespace anof;
using namespace anof::testing::drive;
namespace {
template <class T>
T must(MaintenanceResult<T> result) {
    if (!result) BOOST_FAIL("maintenance error " << static_cast<int>(result.error().code) << " stage " << static_cast<int>(result.error().stage));
    return std::move(result).value();
}
struct Setup {
    Fixture f;
    std::filesystem::path work;
    std::atomic<bool> cancelled{ false };
    MaintenanceContext call{ OperationId::parse("12345678-1234-4234-8234-123456789abc").value(), cancelled };
    std::unique_ptr<OfflineInstanceAccess> access = take(infrastructure::system::makeLinuxOfflineInstanceAccess());
    std::unique_ptr<MetadataInspection> inspection = take(infrastructure::sqlite::makeSqliteMetadataInspection());
    std::unique_ptr<MetadataRepair> repair = take(infrastructure::sqlite::makeSqliteMetadataRepair());
    std::unique_ptr<MaintenanceFileStore> files = take(infrastructure::storage::makeLocalMaintenanceFileStore());
    std::unique_ptr<MaintenanceIdGenerator> ids = take(infrastructure::crypto::makeSodiumMaintenanceIdGenerator());
    std::unique_ptr<MaintenanceRecovery> recovery = runtime::makeOfflineMaintenanceRecovery();
    infrastructure::crypto::SodiumMaintenanceDigest digest;
    Setup() {
        char name[] = "/tmp/anof-fsck-work-XXXXXX";
        auto* p = mkdtemp(name);
        BOOST_REQUIRE(p);
        work = p;
    }
    ~Setup() {
        std::error_code ec;
        std::filesystem::remove_all(work, ec);
    }
    FsckService service() { return { *access, *inspection, *files, *repair, *recovery, digest, *ids, *f.resources.clock }; }
    FsckResult inspect(bool plan = false, InspectionMode mode = InspectionMode::Quick) { return must(service().inspect(call, { f.root, work, mode, plan })); }
    MaintenancePlan plan(InspectionMode mode = InspectionMode::Quick) {
        auto r = inspect(true, mode);
        BOOST_REQUIRE(r.plan);
        return *r.plan;
    }
    MaintenanceResult<ApplyRepairResult> apply(const MaintenancePlan& p, bool confirm = true) { return service().apply(call, { f.root, work, work / "original", confirm }, p); }
    std::filesystem::path object(BlobId id) { return f.root / ".anof/objects" / id.toString().substr(0, 2) / id.toString(); }
    void write(const std::filesystem::path& p, std::string_view bytes) {
        if (!std::filesystem::exists(p.parent_path())) BOOST_REQUIRE(::mkdir(p.parent_path().c_str(), 0700) == 0);
        {
            std::ofstream out(p, std::ios::binary);
            out << bytes;
            BOOST_REQUIRE(out.good());
        }
        BOOST_REQUIRE(::chmod(p.c_str(), 0600) == 0);
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(healthy_inspection_and_planning_preserve_source_fingerprint) {
    Setup s;
    auto a = s.inspect();
    BOOST_CHECK(a.report.status() == MaintenanceReportStatus::Healthy);
    auto p = s.plan();
    BOOST_CHECK(p.actions.empty());
    auto b = s.inspect();
    BOOST_CHECK(a.source.metadata == b.source.metadata);
    BOOST_CHECK(a.source.inventory == b.source.inventory);
    s.cancelled = true;
    auto cancelled = s.service().inspect(s.call, { s.f.root, s.work });
    BOOST_REQUIRE(!cancelled);
    BOOST_CHECK(cancelled.error().code == MaintenanceError::Cancelled);
}
BOOST_AUTO_TEST_CASE(missing_blob_is_only_marked_after_confirmed_plan_and_preservation) {
    Setup s;
    auto u = s.f.publish(s.f.createUpload(s.f.personal->rootNodeId, "missing", 3));
    auto p = s.plan();
    BOOST_REQUIRE_EQUAL(p.actions.size(), 1);
    BOOST_CHECK(std::holds_alternative<MarkBlobUnavailable>(p.actions.front().payload));
    auto denied = s.apply(p, false);
    BOOST_REQUIRE(!denied);
    BOOST_CHECK(!std::filesystem::exists(s.work / "original"));
    auto result = must(s.apply(p));
    BOOST_CHECK(result.verification.report.status() == MaintenanceReportStatus::Findings);
    BOOST_CHECK(std::filesystem::exists(s.work / "original/COMPLETE"));
    auto again = must(s.apply(p));
    BOOST_CHECK(again.alreadyCompleted);
    auto r = take(s.f.store().beginDriveRead());
    auto b = take(r->blobLifecycle().record(*u.pendingBlobId));
    BOOST_REQUIRE(b);
    BOOST_CHECK(b->state == BlobState::Unavailable);
    BOOST_CHECK_EQUAL(b->chargedBytes.value(), 3);
}
BOOST_AUTO_TEST_CASE(orphan_quarantine_charges_once_and_repeat_revalidates_files) {
    Setup s;
    auto id = BlobId::parse(take(s.f.resources.transferIds->uploadId()).toString()).value();
    s.write(s.object(id), "orphan");
    auto p = s.plan();
    BOOST_REQUIRE_EQUAL(p.actions.size(), 1);
    auto q = std::get<QuarantineOrphan>(p.actions.front().payload);
    must(s.apply(p));
    BOOST_CHECK(!std::filesystem::exists(s.object(id)));
    BOOST_CHECK(std::filesystem::exists(s.f.root / ".anof/quarantine" / q.destination.toString()));
    must(s.apply(p));
    auto storage = take(s.f.uploads().storage(*s.f.admin));
    BOOST_CHECK_EQUAL(storage.accounting.retained.value(), 6);
    BOOST_CHECK_EQUAL(storage.gc.quarantinedObjects.value(), 1);
    s.write(s.f.root / ".anof/quarantine" / q.destination.toString(), "changed");
    auto changed = s.apply(p);
    BOOST_REQUIRE(!changed);
}
BOOST_AUTO_TEST_CASE(stale_and_tampered_plan_reject_before_preserving_or_writing) {
    Setup s;
    s.f.publish(s.f.createUpload(s.f.personal->rootNodeId, "missing", 3));
    auto p = s.plan();
    auto forged = p;
    std::get<MarkBlobUnavailable>(forged.actions[0].payload).revision = Revision::from(42).value();
    auto bad = s.apply(forged);
    BOOST_REQUIRE(!bad);
    BOOST_CHECK(bad.error().code == MaintenanceError::InvalidPlan);
    s.f.mkdir(*s.f.admin, *s.f.personal, s.f.personal->rootNodeId, "new");
    auto stale = s.apply(p);
    BOOST_REQUIRE(!stale);
    BOOST_CHECK(stale.error().code == MaintenanceError::StalePlan);
    BOOST_CHECK(!std::filesystem::exists(s.work / "original"));
}
BOOST_AUTO_TEST_CASE(upload_unconfirmed_tail_is_truncated_without_losing_confirmed_bytes) {
    Setup s;
    auto u = s.f.createUpload(s.f.personal->rootNodeId, "upload", 3);
    auto patch = take(s.f.uploads().preparePatch(*s.f.admin, { u.id, {}, ByteCount::from(2).value() }));
    u = take(s.f.uploads().commitPatch(*s.f.admin, patch));
    auto path = s.f.root / ".anof/uploads" / (u.id.toString() + ".part");
    s.write(path, "abc");
    auto p = s.plan();
    BOOST_REQUIRE_EQUAL(p.actions.size(), 1);
    BOOST_CHECK(std::holds_alternative<TruncateUnconfirmedTail>(p.actions[0].payload));
    auto r = must(s.apply(p));
    BOOST_CHECK(r.verification.report.status() == MaintenanceReportStatus::Healthy);
    BOOST_CHECK_EQUAL(std::filesystem::file_size(path), 2);
    must(s.apply(p));
}
BOOST_AUTO_TEST_CASE(quick_skips_hash_full_detects_and_repaired_run_rejects_unrelated_changes) {
    Setup s;
    auto u = s.f.publish(s.f.createUpload(s.f.personal->rootNodeId, "bad-hash", 3));
    s.write(s.object(*u.pendingBlobId), "bad");
    BOOST_CHECK(s.inspect().report.status() == MaintenanceReportStatus::Healthy);
    auto p = s.plan(InspectionMode::Full);
    BOOST_REQUIRE_EQUAL(p.actions.size(), 1);
    BOOST_CHECK(std::get<MarkBlobUnavailable>(p.actions[0].payload).evidence.reason == UnavailableReason::Hash);
    must(s.apply(p));
    s.f.mkdir(*s.f.admin, *s.f.personal, s.f.personal->rootNodeId, "unrelated");
    auto r = s.apply(p);
    BOOST_REQUIRE(!r);
    BOOST_CHECK(r.error().code == MaintenanceError::StalePlan);
}

namespace {
struct LostRecoveryResult final : MaintenanceRecovery {
    std::unique_ptr<MaintenanceRecovery> delegate = runtime::makeOfflineMaintenanceRecovery();
    bool lose = true;
    MaintenanceResult<void> resume(const MaintenanceContext& c, const InstanceLease& l, const ResumeUpload& p) override {
        auto r = delegate->resume(c, l, p);
        if (!r) return r;
        if (lose) {
            lose = false;
            return MaintenanceFailure{ MaintenanceError::PublicationOutcomeUnknown, MaintenanceStage::Recover, c.operationId, MaintenanceOutcome::Unknown, MaintenanceReportStatus::Incomplete, {}, {} };
        }
        return {};
    }
    MaintenanceResult<void> remove(const MaintenanceContext& c, const InstanceLease& l, const ResumeDeletingBlob& p) override { return delegate->remove(c, l, p); }
    MaintenanceResult<void> finalize(const MaintenanceContext& c, const InstanceLease& l, const ResumeDeletingBlob& p) override { return delegate->finalize(c, l, p); }
};
void sql(Setup& s, const std::string& text) {
    auto db = take(infrastructure::sqlite::SqliteConnection::open(s.f.root / ".anof/metadata.sqlite3"));
    BOOST_REQUIRE(db->execute(text));
}
Upload created(Setup& s, std::string name) {
    auto client = OperationId::parse(take(s.f.resources.transferIds->uploadId()).toString()).value();
    return take(s.f.uploads().create(*s.f.admin, { client, { s.f.personal->id, { s.f.personal->rootNodeId, std::move(name) }, FileLength::from(3).value(), {} } }, ByteCount::from(1048576).value(), {})).upload;
}
Upload verifying(Setup& s) {
    auto u = s.f.createUpload(s.f.personal->rootNodeId, "pending", 3);
    s.write(s.f.root / ".anof/uploads" / (u.id.toString() + ".part"), "abc");
    auto patch = take(s.f.uploads().preparePatch(*s.f.admin, { u.id, {}, ByteCount::from(3).value() }));
    u = take(s.f.uploads().commitPatch(*s.f.admin, patch));
    return take(s.f.uploads().acceptComplete(*s.f.admin, { u.id, {} }));
}
}  // namespace
BOOST_AUTO_TEST_CASE(creating_upload_unknown_file_result_reopens_same_run_without_double_effects) {
    Setup s;
    auto u = created(s, "creating");
    s.recovery = std::make_unique<LostRecoveryResult>();
    auto p = s.plan();
    BOOST_REQUIRE_EQUAL(p.actions.size(), 1);
    auto failed = s.apply(p);
    BOOST_REQUIRE(!failed);
    BOOST_CHECK(failed.error().outcome == MaintenanceOutcome::Unknown);
    auto open = take(s.f.uploads().read(u.id));
    BOOST_CHECK(open.state == UploadState::Open);
    BOOST_CHECK(!s.inspect(true).plan);
    auto result = must(s.apply(p));
    BOOST_CHECK(result.verification.report.status() == MaintenanceReportStatus::Healthy);
    BOOST_CHECK(must(s.apply(p)).alreadyCompleted);
    auto r = take(s.f.store().beginDriveRead());
    BOOST_CHECK_EQUAL(take(r->storage().get()).reserved.value(), 3);
}
BOOST_AUTO_TEST_CASE(verifying_and_committing_uploads_recover_only_the_planned_upload) {
    for (bool committing : { false, true }) {
        BOOST_TEST_CONTEXT("committing " << committing) {
            Setup s;
            auto u = verifying(s);
            if (committing) {
                u = take(s.f.uploads().recordVerification(u.id, u.generation, take(s.digest.digest("abc")), request()));
                u = take(s.f.uploads().preparePublish(u.id, u.generation));
            }
            auto p = s.plan(InspectionMode::Full);
            BOOST_REQUIRE_EQUAL(p.actions.size(), 1);
            auto result = must(s.apply(p));
            BOOST_CHECK(result.verification.report.status() == MaintenanceReportStatus::Healthy);
            auto row = take(s.f.uploads().read(u.id));
            BOOST_CHECK(row.state == (committing ? UploadState::Completed : UploadState::Ready));
            BOOST_CHECK(must(s.apply(p)).alreadyCompleted);
        }
    }
}
BOOST_AUTO_TEST_CASE(accounting_repair_and_orphan_quarantine_share_no_stale_ledger_proof) {
    Setup s;
    auto id = BlobId::parse(take(s.f.resources.transferIds->uploadId()).toString()).value();
    s.write(s.object(id), "orphan");
    sql(s, "UPDATE storage_accounting SET retained_bytes=9,revision=revision+1");
    auto p = s.plan();
    BOOST_REQUIRE_EQUAL(p.actions.size(), 2);
    BOOST_CHECK(std::holds_alternative<RecomputeAccounting>(p.actions[0].payload));
    must(s.apply(p));
    BOOST_CHECK_EQUAL(take(s.f.uploads().storage(*s.f.admin)).accounting.retained.value(), 6);
    must(s.apply(p));
}
BOOST_AUTO_TEST_CASE(deleting_cleanup_keeps_intent_until_physical_removal_and_final_accounting) {
    Setup s;
    auto u = s.f.publish(s.f.createUpload(s.f.personal->rootNodeId, "old", 3));
    s.write(s.object(*u.pendingBlobId), "abc");
    auto node = s.f.node(*u.resultNodeId);
    auto entry = take(s.f.service().execute(*s.f.admin, TrashNodeCommand{ node.metadata.id, NodeCondition{ node.metadata.id, node.metadata.revision } })).entry;
    BOOST_REQUIRE(s.f.service().execute(*s.f.admin, PurgeTrashCommand{ entry.id, TrashCondition{ entry.id, entry.revision } }));
    struct FutureClock : Clock {
        UtcTime value;
        explicit FutureClock(UtcTime t) : value(t) {}
        UtcTime now() const override { return value; }
    } clock(s.f.resources.clock->now() + std::chrono::hours(48));
    BlobRecord b = [&] {auto r=take(s.f.store().beginDriveRead());return *take(r->blobLifecycle().record(*u.pendingBlobId)); }();
    MaintenanceService service(s.f.store(), clock);
    take(service.mark(b.content.id, b.revision, request()));
    auto p = s.plan();
    BOOST_REQUIRE_EQUAL(p.actions.size(), 1);
    must(s.apply(p));
    BOOST_CHECK(!std::filesystem::exists(s.object(b.content.id)));
    BOOST_CHECK_EQUAL(take(s.f.uploads().storage(*s.f.admin)).accounting.retained.value(), 0);
    must(s.apply(p));
}
BOOST_AUTO_TEST_CASE(reentry_refuses_damaged_original_backup_and_changed_configuration) {
    {
        Setup s;
        auto u = created(s, "creating");
        (void)u;
        s.recovery = std::make_unique<LostRecoveryResult>();
        auto p = s.plan();
        BOOST_REQUIRE(!s.apply(p));
        s.write(s.work / "original/COMPLETE", "broken");
        BOOST_REQUIRE(!s.apply(p));
    }
    {
        Setup s;
        auto u = created(s, "creating");
        (void)u;
        s.recovery = std::make_unique<LostRecoveryResult>();
        auto p = s.plan();
        BOOST_REQUIRE(!s.apply(p));
        std::ofstream out(s.f.root / ".anof/config.toml", std::ios::app);
        out << "\n# changed externally\n";
        out.close();
        auto r = s.apply(p);
        BOOST_REQUIRE(!r);
        BOOST_CHECK(r.error().code == MaintenanceError::StalePlan);
    }
}
BOOST_AUTO_TEST_CASE(completed_plan_revalidates_blob_metadata_and_never_reapplies_mutation) {
    Setup s;
    auto u = s.f.publish(s.f.createUpload(s.f.personal->rootNodeId, "missing", 3));
    auto p = s.plan();
    must(s.apply(p));
    sql(s, "UPDATE blobs SET revision=revision+1 WHERE id='" + u.pendingBlobId->toString() + "'");
    auto r = s.apply(p);
    BOOST_REQUIRE(!r);
}

BOOST_AUTO_TEST_CASE(restored_content_requires_full_hash_and_explicit_new_plan) {
    Setup s;
    auto u = verifying(s);
    u = take(s.f.uploads().recordVerification(u.id, u.generation, take(s.digest.digest("abc")), request()));
    u = take(s.f.uploads().preparePublish(u.id, u.generation));
    auto path = s.object(*u.pendingBlobId);
    s.write(path, "abc");
    BOOST_REQUIRE(std::filesystem::remove(s.f.root / ".anof/uploads" / (u.id.toString() + ".part")));
    u = take(s.f.uploads().markObject(u.id, u.generation));
    u = take(s.f.uploads().commitPublish(u.id, u.generation, request()));
    s.write(path, "bad");
    must(s.apply(s.plan(InspectionMode::Full)));
    s.write(path, "abc");
    auto quick = s.plan();
    BOOST_CHECK(quick.actions.empty());
    auto full = s.plan(InspectionMode::Full);
    BOOST_REQUIRE_EQUAL(full.actions.size(), 1);
    BOOST_CHECK(std::holds_alternative<VerifyRestoredBlob>(full.actions[0].payload));
    auto r = must(s.service().apply(s.call, { s.f.root, s.work, s.work / "before-restoration", true }, full));
    BOOST_CHECK(r.verification.report.status() == MaintenanceReportStatus::Healthy);
}
BOOST_AUTO_TEST_CASE(sink_cancellation_cannot_produce_executable_plan_or_modify_source) {
    Setup s;
    s.f.publish(s.f.createUpload(s.f.personal->rootNodeId, "missing", 3));
    auto before = s.inspect();
    auto r = s.service().inspect(s.call, { s.f.root, s.work, InspectionMode::Quick, true }, [&](const auto&) -> MaintenanceResult<void> {s.cancelled=true;return {}; });
    BOOST_REQUIRE(!r);
    BOOST_CHECK(r.error().code == MaintenanceError::Cancelled);
    s.cancelled = false;
    auto after = s.inspect();
    BOOST_CHECK(before.source.metadata == after.source.metadata);
    BOOST_CHECK(before.source.inventory == after.source.inventory);
}
BOOST_AUTO_TEST_CASE(unsafe_inventory_never_produces_executable_plan) {
    Setup s;
    s.write(s.f.root / ".anof/uploads/not-an-upload", "unsafe");
    auto r = s.inspect(true);
    BOOST_CHECK(r.report.status() != MaintenanceReportStatus::Healthy);
    BOOST_CHECK(!r.plan);
}
