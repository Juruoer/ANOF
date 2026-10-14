#define BOOST_TEST_MODULE AnofSqliteInstanceBootstrap
#include <boost/test/unit_test.hpp>
#include <anof/infrastructure/persistence/sqlite/tests/BootstrapFixture.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteFaultVfs.h>
#include <anof/infrastructure/persistence/sqlite/SqliteBootstrapInternal.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/system/LinuxInstanceInitializer.h>
#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <anof/infrastructure/config/internal/ConfigurationText.h>
#include <anof/application/maintenance/PrepareInstance.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#include <iterator>

namespace anof::infrastructure::sqlite {
namespace {
using testing::bootstrapBudget;
using testing::bootstrapSeed;
using testing::bootstrapId;
template <typename T> void failure(const InitResult<T>& result, InitError code, InitStage stage) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
    BOOST_CHECK(result.error().stage == stage);
}
struct Fixture {
    std::filesystem::path root;
    BootstrapSeed seed = bootstrapSeed();
    std::unique_ptr<InstanceInitializer> initializer;
    std::unique_ptr<InitializationLease> lease;
    std::unique_ptr<InstanceBootstrap> bootstrap;
    std::unique_ptr<InstanceMaintenance> maintenance;
    Fixture() {
        char name[] = "/tmp/anof-bootstrap-XXXXXX";
        const auto* made = ::mkdtemp(name);
        BOOST_REQUIRE(made);
        root = made;
        initializer = std::move(system::makeLinuxInstanceInitializer()).value();
        auto created = initializer->create(root, seed.identity);
        BOOST_REQUIRE(created);
        lease = std::move(created).value();
        InstanceConfiguration config;
        config.developmentHttp = true;
        BOOST_REQUIRE(lease->writeConfiguration(config));
        bootstrap = std::move(makeSqliteInstanceBootstrap({ bootstrapBudget, { 0 } })).value();
        maintenance = std::move(makeSqliteInstanceMaintenance({ bootstrapBudget, { 0 } })).value();
    }
    ~Fixture() { lease.reset(); std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path path(const char* name) const { return root / ".anof" / name; }
    InitResult<InitReceipt> create() { return bootstrap->createCurrent(lease->lease(), seed); }
    InitResult<InitReceipt> verify() { return bootstrap->verifyComplete(lease->lease(), seed.identity); }
    std::unique_ptr<SqliteConnection> raw() {
        auto db = SqliteConnection::open(path("metadata.sqlite3"));
        BOOST_REQUIRE(db);
        return std::move(db).value();
    }
    void finish() { BOOST_REQUIRE(verify()); BOOST_REQUIRE(lease->publishFormat()); BOOST_REQUIRE(lease->clearPending()); }
    std::string read(const char* name) {
        std::ifstream file(path(name), std::ios::binary);
        return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }
    void write(const char* name, const std::string& text) {
        std::ofstream file(path(name)); file << text; file.close(); BOOST_REQUIRE(file);
        BOOST_REQUIRE_EQUAL(::chmod(path(name).c_str(), 0600), 0);
    }
    std::int64_t scalar(const char* sql) {
        auto db = raw(); auto statement = SqliteStatement::prepare(*db, sql); BOOST_REQUIRE(statement);
        auto row = statement.value()->step(); BOOST_REQUIRE(row && row.value());
        return statement.value()->integer(0).value();
    }
};
std::size_t descriptors() { return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{})); }
int faultMode = 0;
testing::SqliteFaultVfs* faultVfs = nullptr;
Result<void> inject(SqliteConnection& db, detail::BootstrapPoint point) {
    if (faultMode == 1 && point == detail::BootstrapPoint::BeforeAudit) return ErrorCode::StorageFull;
    if (faultMode == 2 && point == detail::BootstrapPoint::Begun)
        return db.execute("CREATE TEMP TRIGGER fail_audit BEFORE INSERT ON identity_audit_events BEGIN SELECT RAISE(ABORT,'test'); END");
    if (faultMode == 3 && point == detail::BootstrapPoint::BeforeCommit) {
        faultVfs->arm(testing::SqliteFaultVfs::Failure::WalSync);
    }
    if (faultMode == 4 && point == detail::BootstrapPoint::BeforeCommit) ::_exit(0);
    return {};
}
}

BOOST_AUTO_TEST_CASE(real_bootstrap_finish_and_prepare_preserve_lock_and_exact_billing) {
    Fixture f;
    const auto count = descriptors();
    auto receipt = f.create(); BOOST_REQUIRE(receipt);
    BOOST_CHECK(receipt.value().adminUserId == f.seed.adminUserId);
    BOOST_CHECK(receipt.value().publicSpaceId == f.seed.publicSpaceId);
    BOOST_CHECK(receipt.value().privateSpaceId == f.seed.privateSpaceId);
    BOOST_CHECK_EQUAL(descriptors(), count);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM users"), 1);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM spaces"), 2);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM nodes"), 2);
    BOOST_CHECK_EQUAL(f.scalar("SELECT sum(used_nodes) FROM space_quotas"), 2);
    BOOST_CHECK_EQUAL(f.scalar("SELECT sum(used_nodes) FROM public_contribution_quotas"), 1);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM identity_audit_events WHERE action='instance_initialized'"), 1);
    BOOST_CHECK_EQUAL(f.scalar("SELECT (SELECT count(*) FROM sessions)+(SELECT count(*) FROM invites)"), 0);
    BOOST_CHECK(!f.maintenance->verify(f.lease->lease()));
    failure(f.create(), InitError::AlreadyExists, InitStage::CreateDatabase);
    for (int i = 0; i < 3; ++i) BOOST_REQUIRE(f.verify());
    BOOST_REQUIRE(f.lease->publishFormat());
    BOOST_REQUIRE(f.verify());
    BOOST_CHECK(!f.maintenance->verify(f.lease->lease()));
    BOOST_REQUIRE(f.lease->clearPending());
    BOOST_REQUIRE(f.maintenance->verify(f.lease->lease()));
    failure(f.verify(), InitError::IncompleteInitialization, InitStage::Verify);
    BOOST_CHECK_EQUAL(descriptors(), count);
    f.lease.reset();
    auto locks = system::makeLinuxInstanceLockProvider(); auto configs = config::makeTomlInstanceConfigurationLoader();
    auto prepared = prepareInstance(f.root, *locks.value(), *configs.value(), *f.maintenance);
    BOOST_REQUIRE(prepared);
    auto store = openSqliteMetadataStore(f.path("metadata.sqlite3"), { bootstrapBudget, {} }); BOOST_REQUIRE(store);
    BOOST_CHECK(!std::filesystem::exists(f.root / "objects"));
}
BOOST_AUTO_TEST_CASE(options_and_invalid_seed_fail_before_database_creation) {
    failure(makeSqliteInstanceBootstrap({}), InitError::InvalidInput, InitStage::Validate);
    BOOST_CHECK(!makeSqliteInstanceMaintenance({}));
    Fixture f;
    f.seed.publicRootId = f.seed.privateRootId;
    failure(f.create(), InitError::InvalidInput, InitStage::Validate);
    BOOST_CHECK(!std::filesystem::exists(f.path("metadata.sqlite3")));
}
BOOST_AUTO_TEST_CASE(password_budget_is_checked_before_creation_and_on_maintenance_reads) {
    Fixture f;
    auto limited = makeSqliteInstanceBootstrap({ { 4096, 1, 1 }, {} }); BOOST_REQUIRE(limited);
    failure(limited.value()->createCurrent(f.lease->lease(), f.seed), InitError::InvalidInput, InitStage::Validate);
    BOOST_CHECK(!std::filesystem::exists(f.path("metadata.sqlite3")));
    BOOST_REQUIRE(f.create());
    failure(limited.value()->verifyComplete(f.lease->lease(), f.seed.identity), InitError::IntegrityError, InitStage::Verify);
    f.finish();
    auto maintenance = makeSqliteInstanceMaintenance({ { 4096, 1, 1 }, {} }); BOOST_REQUIRE(maintenance);
    const auto checked = maintenance.value()->verify(f.lease->lease()); BOOST_REQUIRE(!checked);
    BOOST_CHECK(checked.error().code == ErrorCode::IntegrityError);
}
BOOST_AUTO_TEST_CASE(strict_creation_permissions_and_unsafe_database_files_preserve_evidence) {
    Fixture f;
    const mode_t previous = ::umask(0777);
    auto created = f.create();
    ::umask(previous);
    failure(created, InitError::IntegrityError, InitStage::CreateDatabase);
    struct stat status{}; BOOST_REQUIRE_EQUAL(::stat(f.path("metadata.sqlite3").c_str(), &status), 0);
    BOOST_CHECK_EQUAL(status.st_mode & 07777, 0);
    BOOST_REQUIRE_EQUAL(::chmod(f.path("metadata.sqlite3").c_str(), 0600), 0);
    std::filesystem::remove(f.path("metadata.sqlite3"));
    std::filesystem::create_symlink("config.toml", f.path("metadata.sqlite3"));
    failure(f.verify(), InitError::IntegrityError, InitStage::Verify);
    failure(f.create(), InitError::AlreadyExists, InitStage::CreateDatabase);
}
BOOST_AUTO_TEST_CASE(existing_empty_database_or_orphan_sidecars_are_preserved) {
    for (const char* name : { "metadata.sqlite3", "metadata.sqlite3-wal", "metadata.sqlite3-shm", "metadata.sqlite3-journal" }) {
        Fixture f; f.write(name, "");
        failure(f.create(), InitError::AlreadyExists, InitStage::CreateDatabase);
        BOOST_CHECK(std::filesystem::exists(f.path(name)));
        BOOST_CHECK_EQUAL(std::filesystem::file_size(f.path(name)), 0);
    }
}
BOOST_AUTO_TEST_CASE(unbootstrapped_database_is_not_finishable_or_servable) {
    Fixture f; BOOST_REQUIRE(createSqliteDatabase(f.path("metadata.sqlite3")));
    failure(f.verify(), InitError::IncompleteInitialization, InitStage::Verify);
    BOOST_REQUIRE(f.lease->publishFormat()); BOOST_REQUIRE(f.lease->clearPending());
    auto result = f.maintenance->verify(f.lease->lease()); BOOST_REQUIRE(!result); BOOST_CHECK(result.error().code == ErrorCode::IntegrityError);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM users"), 0);
}
BOOST_AUTO_TEST_CASE(finish_rejects_changed_identity_configuration_and_versions_without_repair) {
    Fixture f; BOOST_REQUIRE(f.create());
    auto other = f.seed.identity; other.operationId = bootstrapId<OperationId>(8);
    failure(f.bootstrap->verifyComplete(f.lease->lease(), other), InitError::IntegrityError, InitStage::Verify);
    auto pending = f.read("initialization.pending");
    f.write("initialization.pending", system::detail::pendingText(other));
    failure(f.bootstrap->verifyComplete(f.lease->lease(), other), InitError::IntegrityError, InitStage::Verify);
    f.write("initialization.pending", pending);
    auto config = f.read("config.toml"); f.write("config.toml", "development_http = true\n");
    failure(f.verify(), InitError::InvalidInput, InitStage::Verify); f.write("config.toml", config);
    for (int version : { 1, 3 }) {
        BOOST_REQUIRE(f.raw()->execute("UPDATE schema_info SET version=" + std::to_string(version)));
        failure(f.verify(), InitError::UnsupportedFormat, InitStage::Verify);
    }
    BOOST_REQUIRE(f.raw()->execute("UPDATE schema_info SET version=2")); BOOST_REQUIRE(f.verify());
}
BOOST_AUTO_TEST_CASE(missing_rows_wrong_audit_and_unbilled_roots_are_not_repaired) {
    for (const char* sql : { "DELETE FROM space_quotas", "DELETE FROM public_contribution_quotas", "DELETE FROM identity_audit_events",
            "UPDATE identity_audit_events SET occurred_at=999", "UPDATE space_quotas SET used_nodes=0", "UPDATE public_contribution_quotas SET used_nodes=0",
            "UPDATE space_quotas SET used_nodes=2" }) {
        Fixture f; BOOST_REQUIRE(f.create()); BOOST_REQUIRE(f.raw()->execute(sql));
        failure(f.verify(), InitError::IntegrityError, InitStage::Verify);
        BOOST_CHECK(std::filesystem::exists(f.path("initialization.pending")));
        BOOST_CHECK(!std::filesystem::exists(f.path("instance.json")));
    }
}
BOOST_AUTO_TEST_CASE(normal_maintenance_accepts_later_revisions_and_rejects_instance_mismatch) {
    Fixture f; BOOST_REQUIRE(f.create()); f.finish();
    BOOST_REQUIRE(f.raw()->execute("UPDATE users SET revision=2,display_name='Changed'; UPDATE space_quotas SET revision=2,used_nodes=2"));
    BOOST_REQUIRE(f.maintenance->verify(f.lease->lease()));
    auto other = f.seed.identity; other.instanceId = bootstrapId<InstanceId>(8);
    f.write("instance.json", system::detail::formatText(other));
    auto checked = f.maintenance->verify(f.lease->lease()); BOOST_REQUIRE(!checked); BOOST_CHECK(checked.error().code == ErrorCode::IntegrityError);
}
BOOST_AUTO_TEST_CASE(all_pending_types_block_startup_before_database_open) {
    Fixture f; BOOST_REQUIRE(f.create()); f.finish();
    const auto before = f.read("metadata.sqlite3");
    for (int kind : { 0, 1, 2 }) {
        if (kind == 0) f.write("initialization.pending", "invalid");
        if (kind == 1) std::filesystem::create_symlink("missing", f.path("initialization.pending"));
        if (kind == 2) BOOST_REQUIRE_EQUAL(::mkfifo(f.path("initialization.pending").c_str(), 0600), 0);
        auto result = f.maintenance->verify(f.lease->lease()); BOOST_REQUIRE(!result); BOOST_CHECK(result.error().code == ErrorCode::IntegrityError);
        const bool unchanged = f.read("metadata.sqlite3") == before;
        BOOST_CHECK(unchanged);
        std::filesystem::remove(f.path("initialization.pending"));
    }
}
BOOST_AUTO_TEST_CASE(write_and_audit_failure_roll_back_all_bootstrap_rows) {
    for (int mode : { 1, 2 }) {
        Fixture f; faultMode = mode;
        f.bootstrap = std::move(detail::makeSqliteInstanceBootstrapWith({ bootstrapBudget, {} }, inject)).value();
        failure(f.create(), mode == 1 ? InitError::StorageFull : InitError::IntegrityError, InitStage::Bootstrap);
        BOOST_CHECK_EQUAL(f.scalar("SELECT (SELECT count(*) FROM users)+(SELECT count(*) FROM instance_settings)+(SELECT count(*) FROM spaces)+(SELECT count(*) FROM nodes)+(SELECT count(*) FROM space_quotas)+(SELECT count(*) FROM public_contribution_quotas)+(SELECT count(*) FROM identity_audit_events)"), 0);
        failure(f.verify(), InitError::IncompleteInitialization, InitStage::Verify);
    }
}
BOOST_AUTO_TEST_CASE(wal_commit_failure_is_not_reported_as_success_or_automatic_retry) {
    Fixture f; testing::SqliteFaultVfs vfs; faultVfs = &vfs; faultMode = 3;
    f.bootstrap = std::move(detail::makeSqliteInstanceBootstrapWith({ bootstrapBudget, {} }, inject)).value();
    const auto result = f.create(); BOOST_REQUIRE(!result); BOOST_CHECK(result.error().stage == InitStage::Bootstrap);
    BOOST_CHECK(result.error().code == InitError::CommitOutcomeUnknown);
    BOOST_CHECK(vfs.triggered());
    BOOST_CHECK(!std::filesystem::exists(f.path("instance.json")));
    BOOST_CHECK(std::filesystem::exists(f.path("initialization.pending")));
    faultVfs = nullptr;
    // 不根据前次退出码猜测是否提交；重新打开后按数据库证据决定能否 finish。
    const auto settings = f.scalar("SELECT count(*) FROM instance_settings");
    BOOST_REQUIRE(settings == 0 || settings == 1);
    failure(f.create(), InitError::AlreadyExists, InitStage::CreateDatabase);
    if (settings == 0) {
        failure(f.verify(), InitError::IncompleteInitialization, InitStage::Verify);
        BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM users"), 0);
        BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM identity_audit_events"), 0);
    } else {
        f.finish();
        BOOST_REQUIRE(f.maintenance->verify(f.lease->lease()));
        BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM identity_audit_events"), 1);
    }
}
BOOST_AUTO_TEST_CASE(process_exit_before_commit_and_after_commit_have_distinct_finish_evidence) {
    for (bool committed : { false, true }) {
        Fixture f;
        f.lease.reset();
        const pid_t child = ::fork(); BOOST_REQUIRE(child >= 0);
        if (child == 0) {
            ::alarm(5);
            auto opened = f.initializer->openPending(f.root); if (!opened) ::_exit(11);
            faultMode = 4;
            auto backend = committed ? makeSqliteInstanceBootstrap({ bootstrapBudget, {} }) : detail::makeSqliteInstanceBootstrapWith({ bootstrapBudget, {} }, inject);
            if (!backend || !backend.value()->createCurrent(opened.value()->lease(), f.seed)) ::_exit(12);
            ::_exit(0);
        }
        int status = 0; BOOST_REQUIRE_EQUAL(::waitpid(child, &status, 0), child); BOOST_REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        auto opened = f.initializer->openPending(f.root); BOOST_REQUIRE(opened); f.lease = std::move(opened).value();
        if (committed) { f.finish(); BOOST_REQUIRE(f.maintenance->verify(f.lease->lease())); }
        else { failure(f.verify(), InitError::IncompleteInitialization, InitStage::Verify); BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM users"), 0); }
    }
}
}
