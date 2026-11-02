#include <tests/persistence_contract/TestMaintenanceBackend.h>
#include <tests/integration/InitializedInstance.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataInspection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataRepair.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataSnapshot.h>
#include <anof/infrastructure/storage/local/LocalMaintenanceFileStore.h>
#include <anof/infrastructure/system/LinuxOfflineInstanceAccess.h>
#include <boost/test/unit_test.hpp>
#include <sys/stat.h>
#include <unistd.h>
namespace anof::testing {
namespace {
template<class R> auto take(R r) { BOOST_REQUIRE(r); return std::move(r).value(); }
class Backend final : public TestMaintenanceBackend {
    std::filesystem::path base_, root_;
    std::optional<InstanceId> instance_;
    std::unique_ptr<OfflineInstanceAccess> access_;
    std::unique_ptr<InstanceLease> lease_;
    std::unique_ptr<MetadataInspection> inspection_;
    std::unique_ptr<MetadataRepair> repair_;
    std::unique_ptr<MetadataSnapshot> snapshot_;
    std::unique_ptr<MaintenanceFileStore> files_;
public:
    Backend() {
        char pattern[] = "/tmp/anof-maintenance-contract-XXXXXX";
        auto p = ::mkdtemp(pattern); BOOST_REQUIRE(p); base_ = p; root_ = base_ / "source";
        BOOST_REQUIRE(::mkdir(root_.c_str(),0700)==0);
        BOOST_REQUIRE(::mkdir((base_/"work").c_str(),0700)==0);
        BOOST_REQUIRE(initializeTestInstance(root_));
        access_=take(infrastructure::system::makeLinuxOfflineInstanceAccess());
        inspection_=take(infrastructure::sqlite::makeSqliteMetadataInspection());
        repair_=take(infrastructure::sqlite::makeSqliteMetadataRepair());
        snapshot_=take(infrastructure::sqlite::makeSqliteMetadataSnapshot(runtime::instancePasswordPolicy().verification));
        files_=take(infrastructure::storage::makeLocalMaintenanceFileStore());
        std::atomic<bool> cancel{false};
        MaintenanceContext call{OperationId::parse("00000000-0000-4000-8000-000000000099").value(),cancel};
        lease_=take(access_->acquireExisting(call,root_));
        auto capture=take(access_->captureInspection(call,*lease_,work()));
        auto reader=take(inspection_->open(call,*capture));
        auto identity=take(reader->identity(call)); BOOST_REQUIRE(identity.instance); instance_=identity.instance;
    }
    ~Backend() { lease_.reset(); std::error_code ec; std::filesystem::remove_all(base_,ec); }
    const std::filesystem::path& root() const override { return root_; }
    std::filesystem::path work() const override { return base_/"work"; }
    const InstanceLease& lease() const override { return *lease_; }
    InstanceId instance() const override { return *instance_; }
    MetadataInspection& inspection() override { return *inspection_; }
    MetadataRepair& repair() override { return *repair_; }
    MetadataSnapshot& snapshot() override { return *snapshot_; }
    OfflineInstanceAccess& access() override { return *access_; }
    MaintenanceFileStore& files() override { return *files_; }
};
}
std::unique_ptr<TestMaintenanceBackend> makeTestMaintenanceBackend() { return std::make_unique<Backend>(); }
}
