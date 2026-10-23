#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestStore.h>
#define BOOST_TEST_MODULE RealInstancePreparation
#include <boost/test/unit_test.hpp>
#include <anof/infrastructure/persistence/sqlite/tests/BootstrapFixture.h>

#include <anof/application/maintenance/PrepareInstance.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace anof::infrastructure::sqlite {
namespace {
namespace fs = std::filesystem;
constexpr const char* kFormat = R"({"instance_id":"12345678-1234-4234-8234-123456789abc","format_version":4})";

template <typename T>
void errorIs(const Result<T>& result, ErrorCode code) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
}

std::size_t fdCount() {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{}));
}

// exec 后锁描述符已关闭；探针独立打开同一锁文件，不能靠继承 fd 假装竞争。
int probe(const fs::path& root, bool success) {
    const std::string path = root.string();
    const pid_t child = ::fork();
    if (child < 0) return -1;
    if (child == 0) {
        ::alarm(5);
        ::execl(INSTANCE_LOCK_PROBE, INSTANCE_LOCK_PROBE, path.c_str(), success ? "1" : "0", nullptr);
        ::_exit(100);
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

struct Fixture {
    fs::path root;
    std::unique_ptr<InstanceLockProvider> locks;
    std::unique_ptr<InstanceConfigurationLoader> configuration;
    std::unique_ptr<InstanceMaintenance> maintenance;
    Fixture() {
        BOOST_REQUIRE_MESSAGE(::geteuid() != 0, "Run filesystem tests as an unprivileged user");
        char pattern[] = "/tmp/anof-preparation-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        BOOST_REQUIRE(created);
        root = created;
        BOOST_REQUIRE(::mkdir((root / ".anof").c_str(), 0700) == 0);
        for (const char* name : { "uploads", "objects", "quarantine" }) BOOST_REQUIRE(::mkdir((root / ".anof" / name).c_str(), 0700) == 0);
        auto lockFactory = system::makeLinuxInstanceLockProvider();
        auto configFactory = config::makeTomlInstanceConfigurationLoader();
        auto maintenanceFactory = makeSqliteInstanceMaintenance({ testing::bootstrapBudget, {} });
        BOOST_REQUIRE(lockFactory && lockFactory.value());
        BOOST_REQUIRE(configFactory && configFactory.value());
        BOOST_REQUIRE(maintenanceFactory && maintenanceFactory.value());
        locks = std::move(lockFactory).value();
        configuration = std::move(configFactory).value();
        maintenance = std::move(maintenanceFactory).value();
        write("config.toml", "development_http = true\nport = 0\n");
        write("instance.json", kFormat);
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path path(const char* name) const { return root / ".anof" / name; }
    void write(const char* name, const std::string& contents) {
        std::ofstream output(path(name), std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        BOOST_REQUIRE(output);
        BOOST_REQUIRE(::chmod(path(name).c_str(), 0600) == 0);
    }
    std::string read(const char* name) const {
        std::ifstream input(path(name), std::ios::binary);
        BOOST_REQUIRE(input);
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }
    void database() {
        testing::createBootstrappedTestDatabase(path("metadata.sqlite3"));
    }
    Result<PreparedInstance> prepare() { return prepareInstance(root, *locks, *configuration, *maintenance); }
    void released(std::size_t descriptors) {
        BOOST_CHECK_EQUAL(fdCount(), descriptors);
        BOOST_CHECK_EQUAL(probe(root, true), 0);
    }
};

// 只观测调用边界，实际读取和校验仍委托生产适配器，保留真实 lease 身份。
struct Observed : InstanceConfigurationLoader, InstanceMaintenance {
    Fixture& fixture;
    std::vector<int> calls;
    int throwAt = 0;
    std::optional<ErrorCode> injectedMaintenanceError;
    bool invalidateValue = false;
    explicit Observed(Fixture& value) : fixture(value) {}
    Result<InstanceConfiguration> load(const InstanceLease& lease) override {
        calls.push_back(1);
        errorIs(fixture.locks->acquire(lease.root()), ErrorCode::Conflict);
        auto result = fixture.configuration->load(lease);
        if (throwAt == 1) throw std::runtime_error("injected configuration boundary failure");
        if (result && invalidateValue) result.value().developmentHttp = false;
        return result;
    }
    Result<void> verify(const InstanceLease& lease) override {
        calls.push_back(2);
        errorIs(fixture.locks->acquire(lease.root()), ErrorCode::Conflict);
        auto result = fixture.maintenance->verify(lease);
        if (throwAt == 2) throw std::runtime_error("injected maintenance boundary failure");
        if (result && injectedMaintenanceError) return *injectedMaintenanceError;
        return result;
    }
    Result<PreparedInstance> prepare() { return prepareInstance(fixture.root, *fixture.locks, *this, *this); }
};
}

BOOST_AUTO_TEST_CASE(real_preparation_transfers_lock_and_worker_opens_its_own_store) {
    Fixture fixture;
    fixture.database();
    fixture.write("instance.lock", "stable-lock-inode");
    struct stat before{}, after{};
    BOOST_REQUIRE(::stat(fixture.path("instance.lock").c_str(), &before) == 0);
    const auto descriptors = fdCount();
    {
        Observed observed(fixture);
        auto result = observed.prepare();
        BOOST_REQUIRE(result);
        BOOST_CHECK(observed.calls == std::vector<int>({ 1, 2 }));
        auto prepared = std::move(result).value();
        BOOST_REQUIRE(prepared.lease);
        BOOST_CHECK(prepared.lease->root() == fixture.root);
        BOOST_CHECK_EQUAL(prepared.configuration.port, 0);
        BOOST_CHECK(prepared.configuration.developmentHttp);
        BOOST_CHECK_EQUAL(prepared.configuration.metadataQueue, 256U);
        BOOST_CHECK_EQUAL(probe(fixture.root, false), 0);
        // 所有维护资源已关闭，只留下 lease 的根目录、.anof 和锁三个描述符。
        BOOST_CHECK_EQUAL(fdCount(), descriptors + 3);
        auto worker = std::async(std::launch::async, [path = fixture.path("metadata.sqlite3")] {
            auto store = openSqliteMetadataStore(path, { testing::bootstrapBudget, {} });
            if (!store) return false;
            auto transaction = store.value()->beginWrite();
            if (!transaction) return false;
            return static_cast<bool>(transaction.value()->commit());
        });
        BOOST_CHECK(worker.get());
        BOOST_CHECK_EQUAL(fdCount(), descriptors + 3);
        BOOST_CHECK_EQUAL(probe(fixture.root, false), 0);
        // 准备结果自有配置和 lease，不依赖三个工厂对象继续存活。
        fixture.locks.reset();
        fixture.configuration.reset();
        fixture.maintenance.reset();
    }
    fixture.released(descriptors);
    BOOST_REQUIRE(::stat(fixture.path("instance.lock").c_str(), &after) == 0);
    BOOST_CHECK_EQUAL(before.st_ino, after.st_ino);
    BOOST_CHECK_EQUAL(before.st_dev, after.st_dev);
    BOOST_CHECK_EQUAL(fixture.read("instance.lock"), "stable-lock-inode");
}

BOOST_AUTO_TEST_CASE(lock_failure_short_circuits_all_file_reading) {
    Fixture fixture;
    auto held = fixture.locks->acquire(fixture.root);
    BOOST_REQUIRE(held);
    fixture.write("config.toml", "not TOML");
    Observed observed(fixture);
    const auto descriptors = fdCount();
    errorIs(observed.prepare(), ErrorCode::Conflict);
    BOOST_CHECK(observed.calls.empty());
    BOOST_CHECK_EQUAL(fdCount(), descriptors);
    BOOST_CHECK_EQUAL(probe(fixture.root, false), 0);
    BOOST_CHECK(!fs::exists(fixture.path("metadata.sqlite3")));
    held.value().reset();
    BOOST_CHECK_EQUAL(probe(fixture.root, true), 0);
    errorIs(fixture.prepare(), ErrorCode::InvalidArgument);
}

BOOST_AUTO_TEST_CASE(rejected_configuration_does_not_modify_existing_database_or_sidecars) {
    Fixture fixture;
    fixture.database();
    const auto database = fixture.read("metadata.sqlite3");
    // 不合法伴随文件是哨兵：配置失败时维护不得触碰或解释它。
    fixture.write("metadata.sqlite3-wal", "untouched-wal");
    fixture.write("metadata.sqlite3-shm", "untouched-shm");
    const auto descriptors = fdCount();
    for (const char* invalid : { "", "development_http=false", "development_http=true\nport=-1", "development_http=true\nunknown=1" }) {
        fixture.write("config.toml", invalid);
        Observed observed(fixture);
        errorIs(observed.prepare(), ErrorCode::InvalidArgument);
        BOOST_CHECK(observed.calls == std::vector<int>({ 1 }));
        BOOST_CHECK(fixture.read("metadata.sqlite3") == database);
        BOOST_CHECK_EQUAL(fixture.read("metadata.sqlite3-wal"), "untouched-wal");
        BOOST_CHECK_EQUAL(fixture.read("metadata.sqlite3-shm"), "untouched-shm");
        fixture.released(descriptors);
    }
}

BOOST_AUTO_TEST_CASE(configuration_file_failures_release_lock_without_creating_database) {
    Fixture fixture;
    const auto descriptors = fdCount();
    fs::remove(fixture.path("config.toml"));
    errorIs(fixture.prepare(), ErrorCode::NotFound);
    fixture.released(descriptors);
    fs::create_symlink(fixture.path("instance.json"), fixture.path("config.toml"));
    errorIs(fixture.prepare(), ErrorCode::IntegrityError);
    fixture.released(descriptors);
    fs::remove(fixture.path("config.toml"));
    fixture.write("config.toml", "development_http=true");
    BOOST_REQUIRE(::chmod(fixture.path("config.toml").c_str(), 0644) == 0);
    errorIs(fixture.prepare(), ErrorCode::IntegrityError);
    fixture.released(descriptors);
    BOOST_CHECK(!fs::exists(fixture.path("metadata.sqlite3")));
    BOOST_CHECK(!fs::exists(fixture.path("metadata.sqlite3-wal")));
}

BOOST_AUTO_TEST_CASE(maintenance_failures_preserve_categories_release_resources_and_allow_retry) {
    Fixture fixture;
    const auto descriptors = fdCount();
    auto reject = [&](ErrorCode expected) {
        Observed observed(fixture);
        errorIs(observed.prepare(), expected);
        BOOST_CHECK(observed.calls == std::vector<int>({ 1, 2 }));
        fixture.released(descriptors);
    };
    fs::remove(fixture.path("instance.json"));
    reject(ErrorCode::NotFound);
    fixture.write("instance.json", R"({"instance_id":"12345678-1234-4234-8234-123456789abc","format_version":1})");
    reject(ErrorCode::IntegrityError);
    BOOST_CHECK(!fs::exists(fixture.path("metadata.sqlite3")));
    fixture.write("instance.json", kFormat);
    reject(ErrorCode::NotFound);
    BOOST_CHECK(!fs::exists(fixture.path("metadata.sqlite3")));
    fixture.write("metadata.sqlite3", "broken database");
    reject(ErrorCode::IntegrityError);
    BOOST_CHECK_EQUAL(fixture.read("metadata.sqlite3"), "broken database");
    fs::remove(fixture.path("metadata.sqlite3"));
    fixture.database();
    {
        auto connection = openSqliteDatabase(fixture.path("metadata.sqlite3"));
        BOOST_REQUIRE(connection);
        BOOST_REQUIRE(connection.value()->execute("UPDATE schema_info SET version=5"));
    }
    const auto futureDatabase = fixture.read("metadata.sqlite3");
    reject(ErrorCode::IntegrityError);
    BOOST_CHECK(fixture.read("metadata.sqlite3") == futureDatabase);
    {
        // 仅测试夹具显式修复；生产准备流程不得自动修改版本。
        auto connection = SqliteConnection::open(fixture.path("metadata.sqlite3"));
        BOOST_REQUIRE(connection);
        BOOST_REQUIRE(connection.value()->execute("UPDATE schema_info SET version=4; CREATE TABLE damaged(value INTEGER CHECK(value>0)); PRAGMA ignore_check_constraints=ON; INSERT INTO damaged VALUES(-1)"));
    }
    reject(ErrorCode::IntegrityError);
    {
        auto connection = SqliteConnection::open(fixture.path("metadata.sqlite3"));
        BOOST_REQUIRE(connection);
        BOOST_REQUIRE(connection.value()->execute("DROP TABLE damaged"));
    }
    {
        auto result = fixture.prepare();
        BOOST_REQUIRE(result);
    }
    fixture.released(descriptors);
}

BOOST_AUTO_TEST_CASE(public_value_revalidation_prevents_maintenance_with_real_lease) {
    Fixture fixture;
    fixture.database();
    const auto before = fixture.read("metadata.sqlite3");
    const auto descriptors = fdCount();
    Observed observed(fixture);
    observed.invalidateValue = true;
    errorIs(observed.prepare(), ErrorCode::InvalidArgument);
    BOOST_CHECK(observed.calls == std::vector<int>({ 1 }));
    BOOST_CHECK(fixture.read("metadata.sqlite3") == before);
    fixture.released(descriptors);
}

BOOST_AUTO_TEST_CASE(injected_boundary_exceptions_release_real_lease_and_maintenance_resources) {
    Fixture fixture;
    fixture.database();
    const auto descriptors = fdCount();
    for (int stage : { 1, 2 }) {
        Observed observed(fixture);
        observed.throwAt = stage;
        errorIs(observed.prepare(), ErrorCode::InternalError);
        BOOST_CHECK_EQUAL(observed.calls.size(), static_cast<std::size_t>(stage));
        fixture.released(descriptors);
        {
            auto result = fixture.prepare();
            BOOST_REQUIRE(result);
        }
        fixture.released(descriptors);
    }
}
BOOST_AUTO_TEST_CASE(injected_backend_outcomes_are_preserved_without_automatic_retry) {
    Fixture fixture;
    fixture.database();
    const auto descriptors = fdCount();
    // 边界注入仅验证编排透传，不声称在只读维护中制造了磁盘满或不确定提交。
    for (auto code : { ErrorCode::Retryable, ErrorCode::StorageFull, ErrorCode::CommitOutcomeUnknown }) {
        Observed observed(fixture);
        observed.injectedMaintenanceError = code;
        errorIs(observed.prepare(), code);
        BOOST_CHECK(observed.calls == std::vector<int>({ 1, 2 }));
        fixture.released(descriptors);
    }
}
}  // namespace anof::infrastructure::sqlite
