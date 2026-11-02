#define BOOST_TEST_MODULE MaintenanceBackendContract
#include <tests/persistence_contract/TestMaintenanceBackend.h>

#include <boost/test/unit_test.hpp>
using namespace anof;
using namespace anof::testing;
namespace {
template <class T>
T id() { return T::parse("00000000-0000-4000-8000-000000000001").value(); }
template <class R>
auto takeAt(R r, const char* expression) {
    BOOST_REQUIRE_MESSAGE(r, expression);
    return std::move(r).value();
}
#define take(expression) takeAt((expression), #expression)
UtcTime now() { return UtcTime{ std::chrono::microseconds{ 1000000 } }; }
MaintenanceRun run(InstanceId instance) {
    return { id<MaintenanceRunId>(), id<MaintenancePlanId>(), instance, Sha256::parse(std::string(64, 'a')).value(), now(), now(), false, {} };
}
}  // namespace
BOOST_AUTO_TEST_CASE(inspection_and_inventory_outlive_creation_context) {
    auto b = makeTestMaintenanceBackend();
    std::unique_ptr<InspectionCapture> capture;
    std::unique_ptr<MetadataInspectionReader> reader;
    std::unique_ptr<MaintenanceInventory> inventory;
    {
        std::atomic<bool> oldFlag{ false };
        MaintenanceContext call{ id<OperationId>(), oldFlag };
        capture = take(b->access().captureInspection(call, b->lease(), b->work()));
        reader = take(b->inspection().open(call, *capture));
        inventory = take(b->files().inventory(call, b->lease(), InspectionMode::Quick));
    }
    std::atomic<bool> flag{ false };
    MaintenanceContext call{ id<OperationId>(), flag };
    BOOST_CHECK(take(reader->summary(call)).status() == MaintenanceReportStatus::Healthy);
    BOOST_CHECK(take(reader->identity(call)).instance == b->instance());
    BOOST_CHECK(take(reader->accounting(call)).complete);
    flag = true;
    BOOST_CHECK(!reader->summary(call));
    BOOST_CHECK(!inventory->next(call, PageSize::from(1).value()));
    flag = false;
    BOOST_CHECK(take(reader->summary(call)).status() == MaintenanceReportStatus::Healthy);
    auto entries = take(inventory->next(call, PageSize::from(1).value()));
    BOOST_CHECK(entries.empty());
}
BOOST_AUTO_TEST_CASE(repair_rollback_commit_and_reopen_are_authoritative) {
    auto b = makeTestMaintenanceBackend();
    std::unique_ptr<MetadataRepairSession> session;
    {
        std::atomic<bool> oldFlag{ false };
        session = take(b->repair().open({ id<OperationId>(), oldFlag }, b->lease()));
    }
    std::atomic<bool> flag{ false };
    MaintenanceContext call{ id<OperationId>(), flag };
    auto value = run(b->instance());
    {
        auto tx = take(session->beginWrite(call));
        BOOST_REQUIRE(tx->insertRun(call, value));
    }
    BOOST_CHECK(!take(session->findRun(call, value.plan)));
    {
        auto tx = take(session->beginWrite(call));
        BOOST_REQUIRE(tx->insertRun(call, value));
        BOOST_REQUIRE(tx->commit(call));
    }
    session.reset();
    session = take(b->repair().open(call, b->lease()));
    auto persisted = take(session->findRun(call, value.plan));
    BOOST_REQUIRE(persisted);
    BOOST_CHECK(persisted->id == value.id);
    BOOST_CHECK(persisted->planDigest == value.planDigest);
}
BOOST_AUTO_TEST_CASE(rejected_call_cannot_commit_prior_writes) {
    auto b = makeTestMaintenanceBackend();
    std::atomic<bool> flag{ false };
    MaintenanceContext call{ id<OperationId>(), flag };
    auto session = take(b->repair().open(call, b->lease()));
    auto value = run(b->instance());
    {
        auto tx = take(session->beginWrite(call));
        BOOST_REQUIRE(tx->insertRun(call, value));
        flag = true;
        auto rejected = tx->commit(call);
        BOOST_REQUIRE(!rejected);
        BOOST_CHECK(rejected.error().code == MaintenanceError::Cancelled);
        flag = false;
        BOOST_CHECK(!tx->commit(call));
    }
    session.reset();
    session = take(b->repair().open(call, b->lease()));
    BOOST_CHECK(!take(session->findRun(call, value.plan)));
}
BOOST_AUTO_TEST_CASE(snapshot_normalization_and_restore_publish_preserve_source_identity) {
    auto b = makeTestMaintenanceBackend();
    std::atomic<bool> flag{ false };
    MaintenanceContext call{ id<OperationId>(), flag };
    const auto target = b->work() / "backup";
    auto destination = take(b->access().acquireDestination(call, b->root(), target));
    BOOST_REQUIRE(b->snapshot().exportConsistent(call, b->lease(), *destination));
    auto exclusions = take(b->snapshot().normalizeBackup(call, *destination));
    auto metadata = take(b->snapshot().verifyBackup(call, *destination));
    InstanceConfiguration config;
    config.developmentHttp = true;
    auto configuration = take(b->files().writeConfiguration(call, *destination, config));
    auto index = take(b->files().finishObjectIndex(call, *destination));
    BackupManifest manifest{ 1, "sqlite", "contract", 5, 5, id<BackupId>(), b->instance(), now(), metadata, configuration, index, exclusions };
    BOOST_REQUIRE(b->files().sealBackup(call, *destination, manifest));
    BOOST_REQUIRE(b->files().publish(call, *destination));
    BOOST_CHECK(!b->access().acquireDestination(call, b->root(), target));
    auto backup = take(b->files().openBackup(call, target));
    BOOST_CHECK(take(backup->objects(call, PageSize::from(1).value(), {})).items.empty());
    auto restored = take(b->access().acquireDestination(call, b->root(), b->work() / "restored"));
    BOOST_REQUIRE(b->files().copyBackup(call, *backup, *restored));
    BOOST_REQUIRE(b->files().writeConfiguration(call, *restored, config));
    BOOST_REQUIRE(b->snapshot().prepareRestore(call, *restored, b->instance()));
    BOOST_REQUIRE(b->files().sealRestoredInstance(call, *restored, b->instance()));
    BOOST_REQUIRE(b->files().publish(call, *restored));
    auto lease = take(b->access().acquireExisting(call, restored->target()));
    auto capture = take(b->access().captureInspection(call, *lease, b->work()));
    auto reader = take(b->inspection().open(call, *capture));
    BOOST_CHECK(take(reader->identity(call)).instance == b->instance());
    BOOST_CHECK(take(reader->summary(call)).status() == MaintenanceReportStatus::Healthy);
}
