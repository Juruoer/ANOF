#define BOOST_TEST_MODULE AnofSqliteAcceptance
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteFaultVfs.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestMetadataBackend.h>

#include <boost/test/unit_test.hpp>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace anof::infrastructure::sqlite::testing {
namespace {

template <typename T>
T id(unsigned value) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setfill('0') << std::setw(12) << value;
    return T::parse(text.str()).value();
}

QuotaDelta amount(std::int64_t nodes) { return { ByteCount{}, NodeCount::from(nodes).value() }; }

struct Scenario {
    UserId user = id<UserId>(1);
    SpaceId space = id<SpaceId>(1);
    NodeId root = id<NodeId>(1);
    NodeId child = id<NodeId>(2);
    UtcTime now{ std::chrono::microseconds{ 1000000 } };
    QuotaScope scope = SpaceQuota{ space };
    QuotaScope contribution = PublicContributionQuota{ space, user };
    anof::testing::MetadataSeed seed{
        { { { user, UserRole::Administrator, AuthEpoch{} }, true } }, {}, { { space, SpaceKind::Public, std::nullopt, root } }, {}, { { { root, space, std::nullopt, "", user, Revision{}, now, now }, std::nullopt, false } }, { { scope, { amount(10), amount(1), amount(0) } }, { contribution, { amount(10), amount(0), amount(0) } } }
    };
};

template <typename T>
void errorIs(const Result<T>& result, ErrorCode expected) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == expected);
}

std::unique_ptr<SqliteConnection> connect(SqliteTestMetadataBackend& backend, int timeout = 0) {
    auto result = openSqliteDatabase(backend.databasePath(), { timeout });
    BOOST_REQUIRE(result);
    return std::move(result).value();
}

std::int64_t scalar(SqliteConnection& connection, std::string_view sql) {
    auto statement = SqliteStatement::prepare(connection, sql);
    BOOST_REQUIRE(statement);
    auto row = statement.value()->step();
    BOOST_REQUIRE(row && row.value());
    auto value = statement.value()->integer(0);
    BOOST_REQUIRE(value);
    return value.value();
}

void stage(MetadataTransaction& transaction, const Scenario& s) {
    BOOST_REQUIRE(transaction.quotas().chargeUsed(s.scope, amount(1)));
    BOOST_REQUIRE(transaction.quotas().chargeUsed(s.contribution, amount(1)));
    BOOST_REQUIRE(transaction.nodes().insertDirectory({ s.child, s.space, s.root, "child", s.user, s.now }));
    BOOST_REQUIRE(transaction.nodes().advanceRevision(s.root, Revision{}, s.now));
    BOOST_REQUIRE(transaction.directoryAudit().append({ s.user, s.space, s.child, s.root, id<RequestId>(1), s.now }));
}

void checkConsistent(SqliteTestMetadataBackend& backend, const Scenario& s) {
    auto child = backend.readNode(s.child);
    auto root = backend.readNode(s.root);
    auto space = backend.readQuota(s.scope);
    auto contribution = backend.readQuota(s.contribution);
    auto audit = backend.readAudit();
    BOOST_REQUIRE(child && root && root.value() && space && contribution && audit);
    const bool committed = child.value().has_value();
    BOOST_TEST(root.value()->metadata.revision.value() == (committed ? 2 : 1));
    BOOST_TEST(space.value().used.nodes.value() == (committed ? 2 : 1));
    BOOST_TEST(contribution.value().used.nodes.value() == (committed ? 1 : 0));
    BOOST_TEST(audit.value().size() == (committed ? 1U : 0U));
    auto connection = connect(backend);
    auto statement = SqliteStatement::prepare(*connection, "PRAGMA integrity_check");
    BOOST_REQUIRE(statement);
    auto row = statement.value()->step();
    BOOST_REQUIRE(row && row.value());
    BOOST_TEST(statement.value()->text(0).value() == "ok");
}

void verifyWalFailure(SqliteFaultVfs::Failure failure, int extendedCode) {
    // VFS 先构造、最后销毁；异常退出也先关闭全部连接再恢复默认 VFS。
    SqliteFaultVfs vfs;
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    {
        auto connection = connect(backend);
        bool active = false;
        SqliteMetadataTransaction transaction(*connection, active, { 8192, 1, 1 });
        BOOST_REQUIRE(transaction.begin());
        stage(transaction, s);
        vfs.arm(failure);
        auto result = transaction.commit();
        BOOST_REQUIRE(vfs.triggered());
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::CommitOutcomeUnknown || result.error().code == ErrorCode::InternalError);
        BOOST_CHECK(connection->diagnostic().operation == SqliteOperation::Commit);
        BOOST_TEST(connection->diagnostic().extendedCode == extendedCode);
        BOOST_CHECK(!active);
        errorIs(transaction.commit(), ErrorCode::InvalidArgument);
        errorIs(transaction.spaces().find(s.space), ErrorCode::InvalidArgument);
        if (result.error().code == ErrorCode::CommitOutcomeUnknown)
            BOOST_CHECK(!connection->usable());
        else
            BOOST_CHECK(!connection->inTransaction());
    }
    // 未自动重试。失败提交可能无法确认结果，但重开后不能出现跨仓储部分提交。
    checkConsistent(backend, s);
}

}  // namespace

BOOST_AUTO_TEST_CASE(wal_reader_snapshot_does_not_block_commit_and_pins_checkpoint) {
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto reader = connect(backend);
    auto writer = connect(backend);
    BOOST_REQUIRE(reader->execute("BEGIN"));
    BOOST_TEST(scalar(*reader, "SELECT count(*) FROM nodes") == 1);
    bool active = false;
    SqliteMetadataTransaction transaction(*writer, active, { 8192, 1, 1 });
    BOOST_REQUIRE(transaction.begin());
    stage(transaction, s);
    BOOST_REQUIRE(transaction.commit());
    BOOST_TEST(scalar(*reader, "SELECT count(*) FROM nodes") == 1);
    BOOST_TEST(scalar(*writer, "SELECT count(*) FROM nodes") == 2);
    int logFrames = 0;
    int checkpointed = 0;
    BOOST_TEST(sqlite3_wal_checkpoint_v2(writer->handle(), "main", SQLITE_CHECKPOINT_TRUNCATE, &logFrames, &checkpointed) == SQLITE_BUSY);
    BOOST_CHECK(logFrames > checkpointed);
    BOOST_REQUIRE(reader->rollback());
    BOOST_TEST(scalar(*reader, "SELECT count(*) FROM nodes") == 2);
    BOOST_TEST(sqlite3_wal_checkpoint_v2(writer->handle(), "main", SQLITE_CHECKPOINT_TRUNCATE, &logFrames, &checkpointed) == SQLITE_OK);
    BOOST_TEST(std::filesystem::file_size(backend.databasePath().string() + "-wal") == 0U);
    checkConsistent(backend, s);
}

BOOST_AUTO_TEST_CASE(bounded_busy_wait_and_migration_contention_preserve_history) {
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto writer = connect(backend);
    auto waiter = connect(backend, 20);
    BOOST_TEST(scalar(*waiter, "PRAGMA busy_timeout") == 20);
    BOOST_REQUIRE(writer->beginWrite());
    errorIs(waiter->beginWrite(), ErrorCode::Retryable);
    errorIs(createSqliteSchema(*waiter), ErrorCode::Retryable);
    BOOST_REQUIRE(writer->rollback());
    BOOST_REQUIRE(createSqliteSchema(*waiter));
    BOOST_TEST(scalar(*waiter, "SELECT count(*) FROM schema_info") == 1);
    BOOST_TEST(scalar(*waiter, "SELECT count(*) FROM nodes") == 1);
    BOOST_CHECK(!waiter->inTransaction());
}

BOOST_AUTO_TEST_CASE(connection_detects_downgraded_sync_and_journal_settings) {
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto connection = connect(backend);
    BOOST_REQUIRE(connection->execute("PRAGMA synchronous=NORMAL"));
    errorIs(connection->verifySettings(), ErrorCode::InternalError);
    errorIs(connection->beginWrite(), ErrorCode::InternalError);
    BOOST_REQUIRE(connection->execute("PRAGMA synchronous=FULL; PRAGMA journal_mode=DELETE"));
    errorIs(connection->verifySettings(), ErrorCode::InternalError);
    BOOST_REQUIRE(connection->execute("PRAGMA journal_mode=WAL"));
    BOOST_REQUIRE(connection->verifySettings());
}

BOOST_AUTO_TEST_CASE(real_locked_statement_maps_to_retryable) {
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto connection = connect(backend);
    BOOST_REQUIRE(connection->execute("CREATE TABLE test_lock(value INTEGER); INSERT INTO test_lock VALUES (1), (2)"));
    auto statement = SqliteStatement::prepare(*connection, "SELECT value FROM test_lock");
    BOOST_REQUIRE(statement);
    auto row = statement.value()->step();
    BOOST_REQUIRE(row && row.value());
    errorIs(connection->execute("DROP TABLE test_lock"), ErrorCode::Retryable);
    BOOST_TEST(connection->diagnostic().extendedCode == SQLITE_LOCKED);
    statement.value().reset();
    BOOST_REQUIRE(connection->execute("DROP TABLE test_lock"));
}

BOOST_AUTO_TEST_CASE(non_database_file_is_rejected_as_integrity_error) {
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    {
        // 仅覆盖本测试独占文件，没有活连接或生产路径。
        std::ofstream file(backend.databasePath(), std::ios::binary | std::ios::trunc);
        file << std::string(4096, 'x');
        BOOST_REQUIRE(file.good());
    }
    errorIs(backend.connect(), ErrorCode::IntegrityError);
}

BOOST_AUTO_TEST_CASE(wal_write_failure_never_becomes_success_or_safe_retry) {
    verifyWalFailure(SqliteFaultVfs::Failure::WalWrite, SQLITE_IOERR_WRITE);
}

BOOST_AUTO_TEST_CASE(wal_sync_failure_never_becomes_success_or_safe_retry) {
    verifyWalFailure(SqliteFaultVfs::Failure::WalSync, SQLITE_IOERR_FSYNC);
}

}  // namespace anof::infrastructure::sqlite::testing
