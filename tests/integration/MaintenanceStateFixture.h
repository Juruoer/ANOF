#pragma once
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataRepair.h>
#include <anof/infrastructure/storage/local/LocalMaintenanceFileStore.h>
#include <anof/infrastructure/system/LinuxOfflineInstanceAccess.h>
#include <sys/stat.h>
#include <tests/integration/DriveFixture.h>
namespace anof::testing::drive {
// 通过真实维护端口标记缺失，不绕过动作证据/事务，也不删除内容文件。
inline void markUnavailable(Fixture& f, BlobId blob, UnavailableEvidence evidence) {
    auto nonce = take(f.resources.transferIds->uploadId()).toString();
    const auto shard = f.root / ".anof/objects" / blob.toString().substr(0, 2);
    if (!std::filesystem::exists(shard)) BOOST_REQUIRE(::mkdir(shard.c_str(), 0700) == 0);
    std::atomic<bool> cancelled{ false };
    MaintenanceContext call{ OperationId::parse(nonce).value(), cancelled };
    auto access = take(infrastructure::system::makeLinuxOfflineInstanceAccess());
    auto locked = access->acquireExisting(call, f.root);
    BOOST_REQUIRE(locked);
    auto repair = take(infrastructure::sqlite::makeSqliteMetadataRepair());
    auto opened = repair->open(call, *locked.value());
    BOOST_REQUIRE(opened);
    BlobRecord b = [&] {auto r=take(f.store().beginDriveRead()); auto row=take(r->blobLifecycle().record(blob)); BOOST_REQUIRE(row); return *row; }();
    const auto time = f.resources.clock->now();
    const auto hash = Sha256::parse(std::string(64, 'b')).value();
    MaintenanceRun run{ MaintenanceRunId::parse(nonce).value(), MaintenancePlanId::parse(nonce).value(), [&] {auto r=take(f.store().beginDriveRead()); auto settings=take(r->settings().get()); BOOST_REQUIRE(settings); return settings->instanceId; }(), hash, time, time, false, {} };
    MarkBlobUnavailable payload{ blob, b.revision, std::move(evidence) };
    MaintenanceAction action{ MaintenanceActionId::parse(nonce).value(), payload, hash };
    {
        auto tx = opened.value()->beginWrite(call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->insertRun(call, run));
        BOOST_REQUIRE(tx.value()->prepareAction(call, run.id, { {}, action, MaintenanceActionState::Prepared, {}, time, time, {} }));
        BOOST_REQUIRE(tx.value()->commit(call));
    }
    auto files = take(infrastructure::storage::makeLocalMaintenanceFileStore());
    MaintenanceEvidenceBinding binding{ run.instance, run.id, run.plan, run.planDigest, action.id, {} };
    auto receipt = files->verifyAction(call, *locked.value(), binding, action);
    BOOST_REQUIRE(receipt);
    {
        auto tx = opened.value()->beginWrite(call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->markUnavailable(call, payload, receipt.value(), time));
        BOOST_REQUIRE(tx.value()->advanceAction(call, run.id, action.id, {}, MaintenanceActionState::Applied, receipt.value(), time));
        BOOST_REQUIRE(tx.value()->commit(call));
    }
    binding.actionRevision = Revision::from(2).value();
    receipt = files->verifyAction(call, *locked.value(), binding, action);
    BOOST_REQUIRE(receipt);
    auto tx = opened.value()->beginWrite(call);
    BOOST_REQUIRE(tx);
    BOOST_REQUIRE(tx.value()->advanceAction(call, run.id, action.id, binding.actionRevision, MaintenanceActionState::Verified, receipt.value(), time));
    BOOST_REQUIRE(tx.value()->completeRun(call, run.id, {}, time));
    BOOST_REQUIRE(tx.value()->commit(call));
}
inline void markMissing(Fixture& f, BlobId blob) {
    markUnavailable(f, blob, { UnavailableReason::Missing, {}, {} });
}
}  // namespace anof::testing::drive
