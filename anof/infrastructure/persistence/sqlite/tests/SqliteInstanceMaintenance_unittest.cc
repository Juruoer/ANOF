#define BOOST_TEST_MODULE AnofSqliteInstanceMaintenance
#include <boost/test/unit_test.hpp>
#include <anof/infrastructure/persistence/sqlite/tests/BootstrapFixture.h>

#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>

#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace anof::infrastructure::sqlite {
namespace {
constexpr const char* kFormat = R"({"instance_id":"12345678-1234-4234-8234-123456789abc","format_version":3})";

template <typename T>
void errorIs(const Result<T>& result, ErrorCode code) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
}

struct Instance {
    std::filesystem::path root;
    std::unique_ptr<InstanceLockProvider> locks;
    std::unique_ptr<InstanceLease> lease;
    std::unique_ptr<InstanceMaintenance> maintenance;
    Instance() {
        char pattern[] = "/tmp/anof-maintenance-XXXXXX";
        auto created = ::mkdtemp(pattern);
        if (!created) throw std::runtime_error("Cannot create test directory");
        root = created;
        BOOST_REQUIRE(::mkdir((root / ".anof").c_str(), 0700) == 0);
        for (const char* name : { "uploads", "objects", "quarantine" }) BOOST_REQUIRE(::mkdir((root / ".anof" / name).c_str(), 0700) == 0);
        auto provider = system::makeLinuxInstanceLockProvider();
        BOOST_REQUIRE(provider);
        locks = std::move(provider).value();
        auto acquired = locks->acquire(root);
        BOOST_REQUIRE(acquired);
        lease = std::move(acquired).value();
        auto factory = makeSqliteInstanceMaintenance({ testing::bootstrapBudget, {} });
        BOOST_REQUIRE(factory && factory.value());
        maintenance = std::move(factory).value();
        write("instance.json", kFormat);
    }
    ~Instance() {
        lease.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path path(const char* name) const { return root / ".anof" / name; }
    void write(const char* name, const std::string& bytes) {
        std::ofstream file(path(name), std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        BOOST_REQUIRE(file);
        BOOST_REQUIRE(::chmod(path(name).c_str(), 0600) == 0);
    }
    void database() {
        testing::createBootstrappedTestDatabase(path("metadata.sqlite3"));
    }
    std::unique_ptr<SqliteConnection> open() {
        auto result = openSqliteDatabase(path("metadata.sqlite3"));
        BOOST_REQUIRE(result);
        return std::move(result).value();
    }
    Result<void> verify() { return maintenance->verify(*lease); }
};

std::size_t fdCount() {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator()));
}

std::int64_t scalar(SqliteConnection& connection, const char* sql) {
    auto statement = SqliteStatement::prepare(connection, sql);
    BOOST_REQUIRE(statement);
    auto row = statement.value()->step();
    BOOST_REQUIRE(row && row.value());
    auto value = statement.value()->integer(0);
    BOOST_REQUIRE(value);
    return value.value();
}
}

BOOST_AUTO_TEST_CASE(valid_instance_closes_resources_and_retains_lease) {
    Instance instance;
    instance.database();
    const auto before = fdCount();
    for (int i = 0; i != 8; ++i) BOOST_REQUIRE(instance.verify());
    BOOST_CHECK_EQUAL(fdCount(), before);
    errorIs(instance.locks->acquire(instance.root), ErrorCode::Conflict);
    {
        auto connection = instance.open();
        BOOST_REQUIRE(connection->beginWrite());
        BOOST_CHECK_EQUAL(scalar(*connection, "SELECT count(*) FROM users"), 1);
        BOOST_CHECK_EQUAL(scalar(*connection, "SELECT count(*) FROM sessions"), 0);
        BOOST_REQUIRE(connection->rollback());
    }
    instance.lease.reset();
    BOOST_REQUIRE(instance.locks->acquire(instance.root));
}

BOOST_AUTO_TEST_CASE(format_is_strict_and_precedes_database_open) {
    Instance instance;
    const std::string id = R"("12345678-1234-4234-8234-123456789abc")";
    const std::string prefix = "{\"instance_id\":" + id + ",\"format_version\":";
    for (const char* value : { "0", "1", "-1", "true", "2.0", "2e0", "\"2\"", "null", "[]", "{}", "18446744073709551616" }) {
        instance.write("instance.json", prefix + value + "}");
        errorIs(instance.verify(), ErrorCode::IntegrityError);
    }
    for (const auto& text : {
             std::string(""), std::string("[]"), std::string("{}"),
             prefix + "1,\"format_version\":1}", prefix + "1,\"instance_id\":" + id + "}",
             prefix + "1,\"unknown\":0}", std::string(kFormat) + "{}", std::string("\xef\xbb\xbf") + kFormat,
             std::string(R"({"instance_id":"00000000-0000-0000-0000-000000000000","format_version":3})"),
             std::string(R"({"instance_id":"12345678-1234-4234-8234-123456789ABC","format_version":3})"),
             std::string(R"({"instance_id":true,"format_version":3})"),
             std::string(R"({"instance_id":{},"format_version":3})"),
             std::string("{\"instance_id\":\"\xff\",\"format_version\":1}"),
             std::string(R"({"instance_id":"12345678-1234-4234-8234-123456789abc"})") }) {
        instance.write("instance.json", text);
        errorIs(instance.verify(), ErrorCode::IntegrityError);
    }
    BOOST_CHECK(!std::filesystem::exists(instance.path("metadata.sqlite3")));
    BOOST_CHECK(!std::filesystem::exists(instance.path("metadata.sqlite3-wal")));
}

BOOST_AUTO_TEST_CASE(format_size_boundary_and_readonly_permission) {
    Instance instance;
    instance.database();
    std::string format(kFormat);
    format.resize(4096, ' ');
    instance.write("instance.json", format);
    BOOST_REQUIRE(::chmod(instance.path("instance.json").c_str(), 0400) == 0);
    BOOST_REQUIRE(instance.verify());
    BOOST_REQUIRE(::chmod(instance.path("instance.json").c_str(), 0600) == 0);
    instance.write("instance.json", format + ' ');
    errorIs(instance.verify(), ErrorCode::IntegrityError);
}

BOOST_AUTO_TEST_CASE(missing_empty_and_non_database_files_are_not_initialized) {
    Instance instance;
    errorIs(instance.verify(), ErrorCode::NotFound);
    BOOST_CHECK(!std::filesystem::exists(instance.path("metadata.sqlite3")));
    for (const auto& bytes : { std::string(), std::string(200, 'x') }) {
        instance.write("metadata.sqlite3", bytes);
        errorIs(instance.verify(), ErrorCode::IntegrityError);
        BOOST_CHECK_EQUAL(std::filesystem::file_size(instance.path("metadata.sqlite3")), bytes.size());
        BOOST_CHECK(!std::filesystem::exists(instance.path("metadata.sqlite3-wal")));
    }
    std::filesystem::remove(instance.path("instance.json"));
    errorIs(instance.verify(), ErrorCode::NotFound);
}

BOOST_AUTO_TEST_CASE(future_schema_is_not_migrated) {
    Instance instance;
    instance.database();
    {
        auto connection = instance.open();
        BOOST_REQUIRE(connection->execute("UPDATE schema_info SET version=4"));
    }
    errorIs(instance.verify(), ErrorCode::IntegrityError);
    auto raw = SqliteConnection::open(instance.path("metadata.sqlite3"));
    BOOST_REQUIRE(raw);
    BOOST_CHECK_EQUAL(scalar(*raw.value(), "SELECT version FROM schema_info"), 4);
}

BOOST_AUTO_TEST_CASE(schema_and_foreign_key_damage_are_rejected) {
    Instance instance;
    instance.database();
    {
        auto connection = instance.open();
        BOOST_REQUIRE(connection->execute("CREATE TABLE foreign_probe(value INTEGER REFERENCES users(id))"));
        BOOST_REQUIRE(connection->execute("PRAGMA foreign_keys=OFF"));
        BOOST_REQUIRE(connection->execute("INSERT INTO foreign_probe VALUES(999)"));
    }
    errorIs(instance.verify(), ErrorCode::IntegrityError);
    auto raw = SqliteConnection::open(instance.path("metadata.sqlite3"));
    BOOST_REQUIRE(raw);
    BOOST_REQUIRE(raw.value()->execute("DROP TABLE foreign_probe; DROP TABLE sessions"));
    raw.value().reset();
    errorIs(instance.verify(), ErrorCode::IntegrityError);
}

BOOST_AUTO_TEST_CASE(full_integrity_check_rejects_invalid_data_with_unchanged_schema) {
    Instance instance;
    instance.database();
    {
        auto connection = instance.open();
        BOOST_REQUIRE(connection->execute("PRAGMA ignore_check_constraints=ON; UPDATE space_quotas SET used_bytes=-1"));
    }
    auto before = fdCount();
    for (int i = 0; i != 8; ++i) errorIs(instance.verify(), ErrorCode::IntegrityError);
    BOOST_CHECK_EQUAL(fdCount(), before);
    auto connection = SqliteConnection::open(instance.path("metadata.sqlite3"));
    BOOST_REQUIRE(connection);
    BOOST_CHECK_EQUAL(scalar(*connection.value(), "SELECT min(used_bytes) FROM space_quotas"), -1);
    BOOST_REQUIRE(connection.value()->beginWrite());
    BOOST_REQUIRE(connection.value()->rollback());
}

BOOST_AUTO_TEST_CASE(unsafe_files_and_sidecars_are_rejected_before_sqlite) {
    Instance instance;
    instance.database();
    for (const char* name : { "instance.json", "metadata.sqlite3" }) {
        auto path = instance.path(name);
        auto saved = path.string() + ".saved";
        std::filesystem::rename(path, saved);
        std::filesystem::create_symlink(saved, path);
        errorIs(instance.verify(), ErrorCode::IntegrityError);
        std::filesystem::remove(path);
        BOOST_REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
        errorIs(instance.verify(), ErrorCode::IntegrityError);
        std::filesystem::remove(path);
        std::filesystem::rename(saved, path);
        BOOST_REQUIRE(::chmod(path.c_str(), 0640) == 0);
        errorIs(instance.verify(), ErrorCode::IntegrityError);
        BOOST_REQUIRE(::chmod(path.c_str(), 0600) == 0);
        std::filesystem::create_hard_link(path, saved);
        errorIs(instance.verify(), ErrorCode::IntegrityError);
        std::filesystem::remove(saved);
    }
    for (const char* name : { "metadata.sqlite3-wal", "metadata.sqlite3-shm", "metadata.sqlite3-journal" }) {
        std::filesystem::create_symlink(instance.path("instance.json"), instance.path(name));
        errorIs(instance.verify(), ErrorCode::IntegrityError);
        std::filesystem::remove(instance.path(name));
        BOOST_REQUIRE(::mkfifo(instance.path(name).c_str(), 0600) == 0);
        errorIs(instance.verify(), ErrorCode::IntegrityError);
        std::filesystem::remove(instance.path(name));
        instance.write(name, "");
        BOOST_REQUIRE(::chmod(instance.path(name).c_str(), 0640) == 0);
        errorIs(instance.verify(), ErrorCode::IntegrityError);
        std::filesystem::remove(instance.path(name));
    }
    instance.write("metadata.sqlite3-journal", "");
    errorIs(instance.verify(), ErrorCode::IntegrityError);
    BOOST_CHECK(std::filesystem::exists(instance.path("metadata.sqlite3-journal")));
    std::filesystem::remove(instance.path("metadata.sqlite3-journal"));
    BOOST_REQUIRE(instance.verify());
}

BOOST_AUTO_TEST_CASE(existing_safe_wal_sidecars_are_accepted) {
    Instance instance;
    instance.database();
    {
        auto connection = instance.open();
        BOOST_REQUIRE(connection->execute("UPDATE users SET revision=7"));
        int persist = 1;
        BOOST_REQUIRE_EQUAL(sqlite3_file_control(connection->handle(), "main", SQLITE_FCNTL_PERSIST_WAL, &persist), SQLITE_OK);
    }
    BOOST_REQUIRE(std::filesystem::exists(instance.path("metadata.sqlite3-wal")));
    BOOST_REQUIRE(std::filesystem::exists(instance.path("metadata.sqlite3-shm")));
    BOOST_REQUIRE(instance.verify());
    auto connection = instance.open();
    BOOST_CHECK_EQUAL(scalar(*connection, "SELECT revision FROM users"), 7);
}

BOOST_AUTO_TEST_CASE(rollback_mode_is_rejected_without_conversion) {
    Instance instance;
    instance.database();
    sqlite3* handle = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_open(instance.path("metadata.sqlite3").c_str(), &handle), SQLITE_OK);
    const int changed = sqlite3_exec(handle, "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr);
    const int closed = sqlite3_close(handle);
    BOOST_REQUIRE_EQUAL(changed, SQLITE_OK);
    BOOST_REQUIRE_EQUAL(closed, SQLITE_OK);
    errorIs(instance.verify(), ErrorCode::IntegrityError);
    std::ifstream file(instance.path("metadata.sqlite3"), std::ios::binary);
    file.seekg(18);
    BOOST_CHECK_EQUAL(file.get(), 1);
    BOOST_CHECK_EQUAL(file.get(), 1);
}

BOOST_AUTO_TEST_CASE(foreign_lease_and_replaced_directory_are_rejected) {
    Instance instance;
    struct ForeignLease : InstanceLease {
        std::filesystem::path path;
        const std::filesystem::path& root() const noexcept override { return path; }
    } foreign;
    foreign.path = instance.root;
    errorIs(instance.maintenance->verify(foreign), ErrorCode::InvalidArgument);
    instance.database();
    std::filesystem::rename(instance.root / ".anof", instance.root / "saved");
    BOOST_REQUIRE(::mkdir((instance.root / ".anof").c_str(), 0700) == 0);
    errorIs(instance.verify(), ErrorCode::IntegrityError);
}
}  // namespace anof::infrastructure::sqlite
