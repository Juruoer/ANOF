#define BOOST_TEST_MODULE InitializeInstanceIntegration
#include <boost/test/unit_test.hpp>
#include <tests/integration/InitializedInstance.h>
#include <anof/application/maintenance/InitializeInstance.h>
#include <anof/application/maintenance/PrepareInstance.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/crypto/SodiumPasswordHasher.h>
#include <anof/infrastructure/crypto/SodiumIdentityIdGenerator.h>
#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceBootstrap.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/system/LinuxInstanceInitializer.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/infrastructure/system/SystemClock.h>
#include <anof/infrastructure/storage/local/LocalTransferFileStore.h>
#include <unistd.h>

using namespace anof;
using namespace anof::testing;
using namespace anof::infrastructure;
namespace {
struct Tree {
    std::filesystem::path root;
    Tree() {
        char pattern[] = "/tmp/anof-root-init-XXXXXX";
        auto* name = ::mkdtemp(pattern);
        BOOST_REQUIRE(name);
        root = name;
    }
    ~Tree() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
struct PublicationFailure : InitializationLease {
    std::unique_ptr<InitializationLease> inner;
    explicit PublicationFailure(std::unique_ptr<InitializationLease> value) : inner(std::move(value)) {}
    const InstanceLease& lease() const override { return inner->lease(); }
    const InitializationIdentity& identity() const override { return inner->identity(); }
    InitResult<void> writeConfiguration(const InstanceConfiguration& config) override { return inner->writeConfiguration(config); }
    InitResult<void> publishFormat() override {
        return InitFailure{InitError::PublicationOutcomeUnknown, InitStage::PublishFormat};
    }
    InitResult<void> clearPending() override { BOOST_FAIL("No cleanup after publication uncertainty"); return {}; }
};
struct FailingPublication : InstanceInitializer {
    InstanceInitializer& inner;
    explicit FailingPublication(InstanceInitializer& value) : inner(value) {}
    InitResult<std::unique_ptr<InitializationLease>> create(const std::filesystem::path& root, const InitializationIdentity& id) override {
        auto result = inner.create(root, id);
        if (!result) return result.error();
        return std::unique_ptr<InitializationLease>(new PublicationFailure(std::move(result).value()));
    }
    InitResult<std::unique_ptr<InitializationLease>> openPending(const std::filesystem::path& root) override { return inner.openPending(root); }
};
}
BOOST_AUTO_TEST_CASE(real_init_password_billing_and_prepare_share_policy) {
    Tree tree;
    auto options = testInitOptions(tree.root);
    options.contentBudget = ByteCount::from(987654).value();
    options.instance.configuration.diskSafety = {ByteCount::from(12345).value(), SafetyPercent::from(17).value()};
    auto result = runtime::initializeInstance(std::move(options), PasswordInput::parse("fixture password only").value());
    BOOST_REQUIRE(result);
    BOOST_CHECK(!std::filesystem::exists(tree.root / ".anof/initialization.pending"));
    auto locks = system::makeLinuxInstanceLockProvider();
    auto config = config::makeTomlInstanceConfigurationLoader();
    auto maintenance = sqlite::makeSqliteInstanceMaintenance({runtime::instancePasswordPolicy().verification, {}});
    BOOST_REQUIRE(locks && config && maintenance);
    auto prepared = prepareInstance(tree.root, *locks.value(), *config.value(), *maintenance.value());
    BOOST_REQUIRE(prepared);
    BOOST_CHECK_EQUAL(prepared.value().configuration.diskSafety.minimum.value(), 12345);
    BOOST_CHECK_EQUAL(prepared.value().configuration.diskSafety.percent.value(), 17);
    auto transfer = sqlite::openSqliteDriveMetadataStore(tree.root / ".anof/metadata.sqlite3", {runtime::instancePasswordPolicy().verification, {}});
    BOOST_REQUIRE(transfer);
    auto transferRead = transfer.value()->beginDriveRead();
    BOOST_REQUIRE(transferRead);
    auto accounting = transferRead.value()->storage().get();
    BOOST_REQUIRE(accounting);
    BOOST_CHECK_EQUAL(accounting.value().budget.value(), 987654);
    BOOST_CHECK_EQUAL(accounting.value().retained.value(), 0);
    BOOST_CHECK_EQUAL(accounting.value().reserved.value(), 0);
    BOOST_CHECK_EQUAL(accounting.value().revision.value(), 1);
    auto nodes = transferRead.value()->nodeAccounting().get();
    BOOST_REQUIRE(nodes);
    BOOST_CHECK_EQUAL(nodes.value().used.value(), 2);
    BOOST_CHECK_EQUAL(nodes.value().reserved.value(), 0);
    BOOST_CHECK_EQUAL(nodes.value().revision.value(), 1);
    auto gc = transferRead.value()->blobLifecycle().gcStatistics();
    BOOST_REQUIRE(gc);
    BOOST_CHECK_EQUAL(gc.value().pendingObjects.value(), 0);
    BOOST_CHECK_EQUAL(gc.value().pendingBytes.value(), 0);
    BOOST_CHECK_EQUAL(gc.value().deletingObjects.value(), 0);
    BOOST_CHECK_EQUAL(gc.value().deletingBytes.value(), 0);
    BOOST_CHECK(!transfer.value()->beginRead());
    BOOST_CHECK(!transfer.value()->beginTransferWrite());
    BOOST_CHECK(!transfer.value()->beginDriveWrite());
    transferRead.value().reset();
    // 真正初始化的目录能够直接交给文件适配器；lease 覆盖全部句柄寿命。
    auto files = storage::openLocalTransferFileStore(tree.root);
    BOOST_REQUIRE(std::holds_alternative<std::unique_ptr<TransferFileStore>>(files));
    auto& fileStore = *std::get<std::unique_ptr<TransferFileStore>>(files);
    for (auto area : {InventoryArea::Uploads, InventoryArea::Objects, InventoryArea::Quarantine}) {
        auto page = fileStore.inventory(area, PageSize::from(1).value(), {});
        BOOST_REQUIRE((std::holds_alternative<Page<InventoryEntry, InventoryCursor>>(page)));
        BOOST_CHECK((std::get<Page<InventoryEntry, InventoryCursor>>(page).items.empty()));
    }
    BOOST_CHECK(!locks.value()->acquire(tree.root));
    auto store = sqlite::openSqliteMetadataStore(tree.root / ".anof/metadata.sqlite3", {runtime::instancePasswordPolicy().verification, {}});
    BOOST_REQUIRE(store);
    auto snapshot = store.value()->beginRead();
    BOOST_REQUIRE(snapshot);
    auto password = snapshot.value()->users().findPasswordById(result.value().adminUserId);
    BOOST_REQUIRE(password && password.value());
    BOOST_CHECK_EQUAL(password.value()->encodedHash.parameters().memoryKiB, 65536U);
    BOOST_CHECK_EQUAL(password.value()->encodedHash.parameters().iterations, 2U);
    for (QuotaScope scope : {QuotaScope{SpaceQuota{result.value().publicSpaceId}},
            QuotaScope{SpaceQuota{result.value().privateSpaceId}},
            QuotaScope{PublicContributionQuota{result.value().publicSpaceId, result.value().adminUserId}}}) {
        auto quota = snapshot.value()->quotas().get(scope);
        BOOST_REQUIRE(quota);
        BOOST_CHECK_EQUAL(quota.value().state.used.nodes.value(), 1);
        BOOST_CHECK_EQUAL(quota.value().revision.value(), 1);
    }
    auto sessions = snapshot.value()->sessions().countActive(result.value().adminUserId, UtcTime{});
    BOOST_REQUIRE(sessions);
    BOOST_CHECK_EQUAL(sessions.value().value(), 0);
    snapshot.value().reset(); // 密码计算前释放读快照。
    auto hasher = crypto::makeSodiumPasswordHasher(runtime::instancePasswordPolicy());
    BOOST_REQUIRE(hasher);
    auto verified = hasher.value()->verify(PasswordInput::parse("fixture password only").value(), password.value()->encodedHash);
    BOOST_REQUIRE(verified);
    BOOST_CHECK(verified.value());
    auto repeated = initializeTestInstance(tree.root);
    BOOST_REQUIRE(!repeated);
    BOOST_CHECK(repeated.error().code == InitError::AlreadyExists);
}
BOOST_AUTO_TEST_CASE(committed_bootstrap_can_finish_without_rehash_or_reseed) {
    Tree tree;
    auto files = system::makeLinuxInstanceInitializer();
    auto database = sqlite::makeSqliteInstanceBootstrap({runtime::instancePasswordPolicy().verification, {}});
    auto hasher = crypto::makeSodiumPasswordHasher(runtime::instancePasswordPolicy());
    auto ids = crypto::makeSodiumIdentityIdGenerator();
    auto clock = system::makeSystemClock();
    auto requests = crypto::makeSodiumRequestIdentity();
    BOOST_REQUIRE(files && database && hasher && ids && clock && requests);
    FailingPublication failing(*files.value());
    InitializeInstance init(failing, *database.value(), *hasher.value(), *ids.value(), *clock.value());
    auto failed = init.execute(testInitOptions(tree.root), PasswordInput::parse("fixture password only").value(), requests.value()->forExchange(0, 1));
    BOOST_REQUIRE(!failed);
    BOOST_CHECK(failed.error().code == InitError::PublicationOutcomeUnknown);
    BOOST_CHECK(std::filesystem::exists(tree.root / ".anof/initialization.pending"));
    auto locks = system::makeLinuxInstanceLockProvider();
    auto config = config::makeTomlInstanceConfigurationLoader();
    auto maintenance = sqlite::makeSqliteInstanceMaintenance({runtime::instancePasswordPolicy().verification, {}});
    BOOST_REQUIRE(locks && config && maintenance);
    BOOST_CHECK(!prepareInstance(tree.root, *locks.value(), *config.value(), *maintenance.value()));
    auto finished = runtime::finishInitialization(tree.root);
    BOOST_REQUIRE(finished);
    BOOST_CHECK(prepareInstance(tree.root, *locks.value(), *config.value(), *maintenance.value()));
    auto repeated = runtime::finishInitialization(tree.root);
    BOOST_REQUIRE(!repeated);
    BOOST_CHECK(repeated.error().code == InitError::IncompleteInitialization);
}
BOOST_AUTO_TEST_CASE(invalid_options_never_create_instance_tree) {
    Tree tree;
    auto options = testInitOptions(tree.root);
    options.instance.quotas.contribution.nodes = NodeCount{};
    auto result = runtime::initializeInstance(std::move(options), PasswordInput::parse("fixture password only").value());
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == InitError::InvalidInput);
    BOOST_CHECK(std::filesystem::is_empty(tree.root));
    BOOST_CHECK(!runtime::finishInitialization("relative-root"));
}
