#define BOOST_TEST_MODULE MaintenanceStartupIntegration
#include <anof/infrastructure/storage/local/LocalTransferFileStore.h>
#include <anof/runtime/UploadExecutor.h>
#include <muduox/net/EventLoop.h>
#include <tests/integration/MaintenanceStateFixture.h>
#include <tests/support/EventLoopWait.h>

#include <fstream>
using namespace anof;
using namespace anof::runtime;
using namespace anof::testing::drive;
namespace {
Result<void> recover(Fixture& f) {
    std::atomic<bool> cancelled{ false };
    MaintenanceContext call{ OperationId::parse(take(f.resources.transferIds->uploadId()).toString()).value(), cancelled };
    auto access = take(infrastructure::system::makeLinuxOfflineInstanceAccess());
    auto lease = access->acquireExisting(call, f.root);
    BOOST_REQUIRE(lease);
    muduox::net::EventLoop loop;
    MetadataExecutor metadata{ loop };
    DiskExecutor disk{ loop };
    UploadExecutor uploads{ loop, metadata, disk, DiskSafetyPolicy{ ByteCount{}, SafetyPercent{} } };
    bool m = false, d = false;
    BOOST_REQUIRE(metadata.start([&] { return makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3"); },
                                 [&](auto r) {BOOST_REQUIRE(r);m=true; }));
    BOOST_REQUIRE(disk.start([&]() -> Result<std::unique_ptr<TransferFileStore>> {
        auto opened=infrastructure::storage::openLocalTransferFileStore(f.root);
        if (auto* value=std::get_if<std::unique_ptr<TransferFileStore>>(&opened)) return std::move(*value);
        return ErrorCode::IntegrityError; }, [&](auto r) {BOOST_REQUIRE(r);d=true; }, {}));
    BOOST_REQUIRE(test::waitFor(loop, [&] { return m && d; }));
    std::optional<Result<void>> result;
    uploads.recover([&](auto r) { result = std::move(r); });
    BOOST_REQUIRE(test::waitFor(loop, [&] { return result.has_value(); }));
    uploads.requestStop([&] { disk.requestStop(); });
    BOOST_REQUIRE(test::waitFor(loop, [&] { return disk.snapshot().stopped; }));
    metadata.requestStop();
    BOOST_REQUIRE(test::waitFor(loop, [&] { return metadata.snapshot().state == MetadataExecutorState::Stopped; }));
    return *result;
}
void write(const std::filesystem::path& path, const std::string& bytes) {
    if (!std::filesystem::exists(path.parent_path())) BOOST_REQUIRE(::mkdir(path.parent_path().c_str(), 0700) == 0);
    {
        std::ofstream out(path, std::ios::binary);
        out << bytes;
        BOOST_REQUIRE(out.good());
    }
    BOOST_REQUIRE(::chmod(path.c_str(), 0600) == 0);
}
std::filesystem::path object(Fixture& f, BlobId id) {
    return f.root / ".anof/objects" / id.toString().substr(0, 2) / id.toString();
}
MaintenanceFileObservation observe(Fixture& f, BlobId id) {
    std::atomic<bool> cancelled{ false };
    MaintenanceContext call{ OperationId::parse(take(f.resources.transferIds->uploadId()).toString()).value(), cancelled };
    auto access = take(infrastructure::system::makeLinuxOfflineInstanceAccess());
    auto lease = access->acquireExisting(call, f.root);
    BOOST_REQUIRE(lease);
    auto files = take(infrastructure::storage::makeLocalMaintenanceFileStore());
    auto found = files->observe(call, *lease.value(), id, InspectionMode::Full);
    BOOST_REQUIRE(found && found.value());
    return *found.value();
}
void rejected(Fixture& f) {
    auto r = recover(f);
    BOOST_REQUIRE(!r);
    BOOST_CHECK(r.error().code == ErrorCode::IntegrityError);
}
QuarantineId quarantine(Fixture& f, bool finish) {
    const auto nonce = take(f.resources.transferIds->uploadId()).toString();
    auto blob = BlobId::parse(nonce).value();
    write(object(f, blob), "orphan");
    auto observed = observe(f, blob);
    std::atomic<bool> cancelled{ false };
    MaintenanceContext call{ OperationId::parse(nonce).value(), cancelled };
    auto access = take(infrastructure::system::makeLinuxOfflineInstanceAccess());
    auto lease = access->acquireExisting(call, f.root);
    BOOST_REQUIRE(lease);
    auto repair = take(infrastructure::sqlite::makeSqliteMetadataRepair());
    auto session = repair->open(call, *lease.value());
    BOOST_REQUIRE(session);
    auto files = take(infrastructure::storage::makeLocalMaintenanceFileStore());
    const auto time = f.resources.clock->now();
    auto instance = [&] {auto r=take(f.store().beginDriveRead());return take(r->settings().get())->instanceId; }();
    auto digest = Sha256::parse(std::string(64, 'b')).value();
    MaintenanceRun run{ MaintenanceRunId::parse(nonce).value(), MaintenancePlanId::parse(nonce).value(), instance, digest, time, time, false, {} };
    QuarantineOrphan payload{ blob, QuarantineId::parse(nonce).value(), observed.evidence, *observed.sha256 };
    MaintenanceAction action{ MaintenanceActionId::parse(nonce).value(), payload, digest };
    QuarantineEntry entry{ payload.destination, payload.source, payload.before, payload.sha256, payload.before.size, QuarantineState::Prepared, run.plan, action.id, time, time, {} };
    {
        auto tx = session.value()->beginWrite(call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->insertRun(call, run));
        BOOST_REQUIRE(tx.value()->prepareAction(call, run.id, { {}, action, MaintenanceActionState::Prepared, {}, time, time, {} }));
        BOOST_REQUIRE(tx.value()->prepareQuarantine(call, entry));
        BOOST_REQUIRE(tx.value()->commit(call));
    }
    if (!finish) return entry.id;
    BOOST_REQUIRE(files->quarantine(call, *lease.value(), entry));
    MaintenanceEvidenceBinding binding{ run.instance, run.id, run.plan, run.planDigest, action.id, {} };
    auto receipt = files->verifyAction(call, *lease.value(), binding, action);
    BOOST_REQUIRE(receipt);
    {
        auto tx = session.value()->beginWrite(call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->holdQuarantine(call, entry.id, {}, receipt.value(), time));
        BOOST_REQUIRE(tx.value()->advanceAction(call, run.id, action.id, {}, MaintenanceActionState::Applied, receipt.value(), time));
        BOOST_REQUIRE(tx.value()->commit(call));
    }
    binding.actionRevision = Revision::from(2).value();
    receipt = files->verifyAction(call, *lease.value(), binding, action);
    BOOST_REQUIRE(receipt);
    auto tx = session.value()->beginWrite(call);
    BOOST_REQUIRE(tx);
    BOOST_REQUIRE(tx.value()->advanceAction(call, run.id, action.id, binding.actionRevision, MaintenanceActionState::Verified, receipt.value(), time));
    BOOST_REQUIRE(tx.value()->completeRun(call, run.id, {}, time));
    BOOST_REQUIRE(tx.value()->commit(call));
    return entry.id;
}
}  // namespace
BOOST_AUTO_TEST_CASE(known_missing_accepts_but_unmarked_or_reappeared_content_rejects) {
    Fixture f;
    auto u = f.publish(f.createUpload(f.personal->rootNodeId, "missing", 3));
    rejected(f);
    markMissing(f, *u.pendingBlobId);
    BOOST_REQUIRE(recover(f));
    write(object(f, *u.pendingBlobId), "abc");
    rejected(f);
}
BOOST_AUTO_TEST_CASE(known_length_and_hash_damage_require_unchanged_file_evidence) {
    for (auto reason : { UnavailableReason::Length, UnavailableReason::Hash }) {
        BOOST_TEST_CONTEXT("reason " << static_cast<int>(reason)) {
            Fixture f;
            auto u = f.publish(f.createUpload(f.personal->rootNodeId, "damaged", 3));
            write(object(f, *u.pendingBlobId), reason == UnavailableReason::Length ? "longer" : "bad");
            auto observed = observe(f, *u.pendingBlobId);
            markUnavailable(f, *u.pendingBlobId, { reason, observed.evidence, reason == UnavailableReason::Hash ? observed.sha256 : std::nullopt });
            BOOST_REQUIRE(recover(f));
            auto storage = take(f.uploads().storage(*f.admin));
            BOOST_CHECK_EQUAL(storage.gc.unavailableBytes.value(), observed.evidence.size.value());
            write(object(f, *u.pendingBlobId), "changed");
            rejected(f);
        }
    }
}
BOOST_AUTO_TEST_CASE(held_quarantine_requires_ledger_file_identity_and_presence) {
    Fixture f;
    auto id = quarantine(f, true);
    BOOST_REQUIRE(recover(f));
    auto storage = take(f.uploads().storage(*f.admin));
    BOOST_CHECK_EQUAL(storage.gc.quarantinedBytes.value(), 6);
    BOOST_CHECK_EQUAL(storage.gc.quarantinedObjects.value(), 1);
    BOOST_CHECK_EQUAL(storage.accounting.retained.value(), 6);
    auto path = f.root / ".anof/quarantine" / id.toString();
    BOOST_REQUIRE(std::filesystem::remove(path));
    rejected(f);
    write(path, "orphan");
    rejected(f);
}
BOOST_AUTO_TEST_CASE(pending_maintenance_and_unregistered_quarantine_fail_closed) {
    {
        Fixture f;
        quarantine(f, false);
        rejected(f);
    }
    {
        Fixture f;
        auto id = take(f.resources.transferIds->uploadId());
        write(f.root / ".anof/quarantine" / id.toString(), "unknown");
        rejected(f);
    }
}
