#define BOOST_TEST_MODULE AnofSqliteFoundation
#include <boost/test/unit_test.hpp>

#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace anof::infrastructure::sqlite {
namespace {

class Database {
public:
    Database() {
        char pattern[] = "/tmp/anof-sqlite-foundation-XXXXXX";
        const char* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("Cannot create test directory");
        directory_ = created;
    }
    ~Database() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    std::filesystem::path path() const { return directory_ / "metadata.sqlite3"; }
    std::unique_ptr<SqliteConnection> raw() {
        auto result = SqliteConnection::open(path(), { 0 }, SqliteOpenMode::Create);
        BOOST_REQUIRE(result);
        return std::move(result).value();
    }
    std::unique_ptr<SqliteConnection> current() {
        BOOST_REQUIRE(createSqliteDatabase(path(), { 0 }));
        auto result = openSqliteDatabase(path(), { 0 });
        BOOST_REQUIRE(result);
        return std::move(result).value();
    }
private:
    std::filesystem::path directory_;
};

template <typename T>
void errorIs(const Result<T>& result, ErrorCode expected) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == expected);
}

std::unique_ptr<SqliteStatement> prepare(SqliteConnection& connection, std::string_view sql) {
    auto result = SqliteStatement::prepare(connection, sql);
    BOOST_REQUIRE(result);
    return std::move(result).value();
}

std::int64_t scalar(SqliteConnection& connection, std::string_view sql) {
    auto statement = prepare(connection, sql);
    auto row = statement->step();
    BOOST_REQUIRE(row && row.value());
    auto value = statement->integer(0);
    BOOST_REQUIRE(value);
    return value.value();
}

constexpr const char* kHash = "$argon2id$v=19$m=8192,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
constexpr const char* kUser = "00000000-0000-4000-8000-000000000001";
constexpr const char* kSpace = "00000000-0000-4000-8000-000000000002";
constexpr const char* kRoot = "00000000-0000-4000-8000-000000000003";
constexpr const char* kChild = "00000000-0000-4000-8000-000000000004";
constexpr const char* kOther = "00000000-0000-4000-8000-000000000005";
constexpr const char* kBlob = "00000000-0000-4000-8000-000000000006";

std::string quoted(const char* value) { return "'" + std::string(value) + "'"; }

Result<void> insertNode(SqliteConnection& connection, const char* id, std::string_view name,
                        bool file = false, bool trashed = false, const char* parent = kRoot) {
    auto statement = prepare(connection,
        "INSERT INTO nodes(id, space_id, parent_id, name, kind, created_by, blob_id, revision, trashed, created_at, updated_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 1, ?8, 0, 0)");
    BOOST_REQUIRE(statement->bindText(1, id));
    BOOST_REQUIRE(statement->bindText(2, kSpace));
    if (parent) BOOST_REQUIRE(statement->bindText(3, parent));
    else BOOST_REQUIRE(statement->bindNull(3));
    BOOST_REQUIRE(statement->bindText(4, name));
    BOOST_REQUIRE(statement->bindInt64(5, file ? 1 : 0));
    BOOST_REQUIRE(statement->bindText(6, kUser));
    if (file) BOOST_REQUIRE(statement->bindText(7, kBlob));
    else BOOST_REQUIRE(statement->bindNull(7));
    BOOST_REQUIRE(statement->bindInt64(8, trashed ? 1 : 0));
    auto row = statement->step();
    if (!row) return row.error();
    BOOST_REQUIRE(!row.value());
    return {};
}

void seed(SqliteConnection& connection) {
    BOOST_REQUIRE(connection.beginWrite());
    BOOST_REQUIRE(connection.execute("INSERT INTO users VALUES (" + quoted(kUser) + ", 1, 1, 0, 'admin', 'Administrator', '" + std::string(kHash) + "', 1, 1, 0, 0)"));
    BOOST_REQUIRE(connection.execute("INSERT INTO spaces VALUES (" + quoted(kSpace) + ", 1, NULL, " + quoted(kRoot) + ")"));
    BOOST_REQUIRE(insertNode(connection, kRoot, "", false, false, nullptr));
    BOOST_REQUIRE(connection.execute("INSERT INTO blobs(id,size,sha256) VALUES (" + quoted(kBlob) + ", 0, zeroblob(32))"));
    BOOST_REQUIRE(connection.commit());
}

}  // namespace

BOOST_AUTO_TEST_CASE(schema_settings_repeat_and_reopen_preserve_data) {
    Database database;
    {
        auto connection = database.current();
        BOOST_REQUIRE(connection->verifySettings());
        BOOST_TEST(scalar(*connection, "PRAGMA foreign_keys") == 1);
        BOOST_TEST(scalar(*connection, "PRAGMA synchronous") == 2);
        BOOST_TEST(scalar(*connection, "SELECT count(*) FROM schema_info") == 1);
        seed(*connection);
        BOOST_REQUIRE(createSqliteSchema(*connection));
        BOOST_TEST(scalar(*connection, "SELECT count(*) FROM users") == 1);
        BOOST_CHECK(!connection->inTransaction());
    }
    errorIs(createSqliteDatabase(database.path()), ErrorCode::Conflict);
    auto reopened = openSqliteDatabase(database.path());
    BOOST_REQUIRE(reopened);
    BOOST_TEST(scalar(*reopened.value(), "SELECT count(*) FROM nodes") == 1);
    BOOST_CHECK(std::filesystem::exists(database.path().string() + "-wal"));
}

BOOST_AUTO_TEST_CASE(open_validates_arguments_and_does_not_implicitly_migrate) {
    Database database;
    errorIs(openSqliteDatabase(database.path()), ErrorCode::InternalError);
    BOOST_CHECK(!std::filesystem::exists(database.path()));
    errorIs(SqliteConnection::open(database.path(), { -1 }, SqliteOpenMode::Create), ErrorCode::InvalidArgument);
    errorIs(SqliteConnection::open(database.path(), { 5001 }, SqliteOpenMode::Create), ErrorCode::InvalidArgument);
    errorIs(SqliteConnection::open(":memory:"), ErrorCode::InvalidArgument);
    errorIs(SqliteConnection::open(std::filesystem::path(std::string("bad\0path", 8))), ErrorCode::InvalidArgument);
    auto connection = database.raw();
    errorIs(openSqliteDatabase(database.path()), ErrorCode::IntegrityError);
    BOOST_TEST(scalar(*connection, "SELECT count(*) FROM sqlite_schema") == 0);
    BOOST_REQUIRE(connection->execute("PRAGMA foreign_keys=OFF"));
    errorIs(connection->verifySettings(), ErrorCode::InternalError);
    errorIs(connection->beginWrite(), ErrorCode::InternalError);
}

BOOST_AUTO_TEST_CASE(statements_preserve_lengths_types_and_do_not_repeat_writes) {
    Database database;
    auto connection = database.raw();
    BOOST_REQUIRE(connection->execute("CREATE TABLE values_test(i, t, b, n)"));
    auto statement = prepare(*connection, "INSERT INTO values_test VALUES (?1, ?2, ?3, ?4)");
    BOOST_REQUIRE(statement->bindInt64(1, std::numeric_limits<std::int64_t>::max()));
    const std::string embedded("a\0b", 3);
    BOOST_REQUIRE(statement->bindText(2, embedded));
    const unsigned char bytes[] = { 0, 255, 1 };
    BOOST_REQUIRE(statement->bindBlob(3, bytes, sizeof(bytes)));
    BOOST_REQUIRE(statement->bindNull(4));
    auto done = statement->step();
    BOOST_REQUIRE(done && !done.value());
    errorIs(statement->step(), ErrorCode::InvalidArgument);
    errorIs(statement->bindNull(1), ErrorCode::InvalidArgument);
    BOOST_TEST(scalar(*connection, "SELECT count(*) FROM values_test") == 1);
    statement = prepare(*connection, "SELECT i, t, b, n FROM values_test");
    errorIs(statement->integer(0), ErrorCode::InvalidArgument);
    auto row = statement->step();
    BOOST_REQUIRE(row && row.value());
    BOOST_TEST(statement->integer(0).value() == std::numeric_limits<std::int64_t>::max());
    BOOST_CHECK(statement->text(1).value() == embedded);
    BOOST_CHECK(statement->blob(2).value() == std::vector<std::uint8_t>(bytes, bytes + 3));
    BOOST_CHECK(statement->isNull(3).value());
    errorIs(statement->integer(1), ErrorCode::IntegrityError);
    errorIs(statement->text(3), ErrorCode::IntegrityError);
    errorIs(statement->blob(1), ErrorCode::IntegrityError);
    errorIs(statement->integer(9), ErrorCode::InvalidArgument);
    errorIs(SqliteStatement::prepare(*connection, "SELECT 1; SELECT 2"), ErrorCode::InvalidArgument);
    errorIs(SqliteStatement::prepare(*connection, std::string("SELECT 1\0", 9)), ErrorCode::InvalidArgument);
    statement = prepare(*connection, "SELECT ?1, ?2");
    BOOST_REQUIRE(statement->bindText(1, {}));
    BOOST_REQUIRE(statement->bindBlob(2, nullptr, 0));
    BOOST_REQUIRE(statement->step());
    BOOST_CHECK(statement->text(0).value().empty());
    BOOST_CHECK(statement->blob(1).value().empty());
}

BOOST_AUTO_TEST_CASE(value_decoding_rejects_corrupt_persistent_values) {
    Database database;
    auto connection = database.raw();
    auto statement = prepare(*connection,
        "SELECT '00000000-0000-4000-8000-00000000000a', '00000000-0000-4000-8000-00000000000A', "
        "'00000000-0000-0000-0000-000000000000', -1, 0, 2, zeroblob(32), zeroblob(31), "
        "1099511627776, 9223372036854775807, 1.5, -9223372036854775808");
    BOOST_REQUIRE(statement->step());
    BOOST_REQUIRE(readId<NodeId>(*statement, 0));
    errorIs(readId<NodeId>(*statement, 1), ErrorCode::IntegrityError);
    errorIs(readId<NodeId>(*statement, 2), ErrorCode::IntegrityError);
    errorIs(readQuantity<ByteCount>(*statement, 3), ErrorCode::IntegrityError);
    errorIs(readQuantity<Revision>(*statement, 4), ErrorCode::IntegrityError);
    errorIs(readBoolean(*statement, 5), ErrorCode::IntegrityError);
    errorIs(readUserRole(*statement, 5), ErrorCode::IntegrityError);
    errorIs(readSpaceKind(*statement, 5), ErrorCode::IntegrityError);
    BOOST_REQUIRE(readSessionDigest(*statement, 6));
    errorIs(readSessionDigest(*statement, 7), ErrorCode::IntegrityError);
    BOOST_TEST(readQuantity<ByteCount>(*statement, 8).value().value() == 1099511627776LL);
    BOOST_TEST(readQuantity<Revision>(*statement, 9).value().value() == std::numeric_limits<std::int64_t>::max());
    errorIs(readQuantity<ByteCount>(*statement, 10), ErrorCode::IntegrityError);
    BOOST_TEST(readTime(*statement, 11).value().time_since_epoch().count() == std::numeric_limits<std::int64_t>::min());
}

BOOST_AUTO_TEST_CASE(schema_rejects_unknown_definition_and_missing_objects) {
    const std::pair<const char*, ErrorCode> changes[] = {
        { "UPDATE schema_info SET version=4", ErrorCode::IntegrityError },
        { "UPDATE schema_info SET version=1", ErrorCode::IntegrityError },
        { "UPDATE schema_info SET definition='changed'", ErrorCode::IntegrityError },
        { "DELETE FROM schema_info", ErrorCode::IntegrityError },
        { "DROP INDEX nodes_active_name", ErrorCode::IntegrityError },
        { "DROP TRIGGER nodes_shape_insert", ErrorCode::IntegrityError }
    };
    for (const auto& change : changes) {
        Database database;
        auto connection = database.current();
        BOOST_REQUIRE(connection->execute(change.first));
        errorIs(verifySqliteSchema(*connection), change.second);
        errorIs(createSqliteSchema(*connection), change.second);
        BOOST_CHECK(!connection->inTransaction());
        errorIs(openSqliteDatabase(database.path()), change.second);
    }
}

BOOST_AUTO_TEST_CASE(unknown_existing_database_is_not_overwritten) {
    Database database;
    auto connection = database.raw();
    BOOST_REQUIRE(connection->execute("CREATE TABLE sqlitex(value); INSERT INTO sqlitex VALUES (42)"));
    errorIs(createSqliteSchema(*connection), ErrorCode::IntegrityError);
    BOOST_TEST(scalar(*connection, "SELECT value FROM sqlitex") == 42);
    BOOST_TEST(scalar(*connection, "SELECT count(*) FROM sqlite_schema WHERE name='schema_info'") == 0);
}

BOOST_AUTO_TEST_CASE(real_mid_creation_failure_rolls_back_all_ddl) {
    Database database;
    auto connection = database.raw();
    // 在最后一张表处拒绝 DDL：先前已建的表、索引和触发器必须一起回滚。
    auto denyAudit = [](void*, int operation, const char* first, const char*, const char*, const char*) -> int {
        return operation == SQLITE_CREATE_TABLE && first && std::string_view(first) == "audit_events" ? SQLITE_DENY : SQLITE_OK;
    };
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(connection->handle(), denyAudit, nullptr), SQLITE_OK);
    errorIs(createSqliteSchema(*connection), ErrorCode::InternalError);
    BOOST_CHECK(!connection->inTransaction());
    BOOST_TEST(scalar(*connection, "SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'") == 0);
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(connection->handle(), nullptr, nullptr), SQLITE_OK);
    BOOST_REQUIRE(createSqliteSchema(*connection));
    BOOST_REQUIRE(verifySqliteSchema(*connection));
}

BOOST_AUTO_TEST_CASE(root_foreign_keys_and_commit_failure_are_atomic) {
    Database database;
    auto connection = database.current();
    BOOST_REQUIRE(connection->beginWrite());
    BOOST_REQUIRE(connection->execute("INSERT INTO users VALUES (" + quoted(kUser) + ", 1, 1, 0, 'admin', 'Administrator', '" + std::string(kHash) + "', 1, 1, 0, 0)"));
    BOOST_REQUIRE(connection->execute("INSERT INTO spaces VALUES (" + quoted(kSpace) + ", 1, NULL, " + quoted(kRoot) + ")"));
    errorIs(connection->commit(), ErrorCode::IntegrityError);
    BOOST_CHECK(!connection->inTransaction());
    BOOST_CHECK(connection->usable());
    BOOST_CHECK(connection->diagnostic().operation == SqliteOperation::Commit);
    BOOST_TEST(connection->diagnostic().extendedCode == SQLITE_CONSTRAINT_FOREIGNKEY);
    BOOST_TEST(scalar(*connection, "SELECT count(*) FROM users") == 0);
    seed(*connection);
    errorIs(connection->execute("UPDATE nodes SET trashed=1"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE nodes SET name='renamed'"), ErrorCode::IntegrityError);
    errorIs(connection->execute("DELETE FROM nodes"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE spaces SET root_node_id=" + quoted(kChild)), ErrorCode::IntegrityError);
    errorIs(insertNode(*connection, kChild, "", false, false, nullptr), ErrorCode::IntegrityError);
    errorIs(insertNode(*connection, kChild, "missing", false, false, kOther), ErrorCode::IntegrityError);
    BOOST_REQUIRE(insertNode(*connection, kChild, "file", true));
    errorIs(insertNode(*connection, kOther, "under-file", false, false, kChild), ErrorCode::IntegrityError);
    BOOST_REQUIRE(verifySqliteSchema(*connection));
}

BOOST_AUTO_TEST_CASE(active_names_share_namespace_and_preserve_bytes) {
    Database database;
    auto connection = database.current();
    seed(*connection);
    BOOST_REQUIRE(insertNode(*connection, kChild, "child", true));
    errorIs(insertNode(*connection, kOther, "child"), ErrorCode::IntegrityError);
    BOOST_TEST(connection->diagnostic().extendedCode == SQLITE_CONSTRAINT_UNIQUE);
    BOOST_REQUIRE(insertNode(*connection, kOther, "Child"));
    BOOST_REQUIRE(connection->execute("DELETE FROM nodes WHERE parent_id IS NOT NULL"));
    BOOST_REQUIRE(insertNode(*connection, kChild, "child", false, true));
    BOOST_REQUIRE(insertNode(*connection, kOther, "child"));
    BOOST_REQUIRE(connection->execute("DELETE FROM nodes WHERE parent_id IS NOT NULL"));
    for (const auto& name : { std::string(""), std::string(".."), std::string("a/b"), std::string("a\\b"), std::string("a\0b", 3), std::string(256, 'a') }) {
        errorIs(insertNode(*connection, kChild, name), ErrorCode::IntegrityError);
    }
    BOOST_REQUIRE(insertNode(*connection, kChild, std::string(255, 'a')));
    // 未标记后代可隶属回收站祖先；可见性由后续仓储沿祖先链检查。
    BOOST_REQUIRE(connection->execute("UPDATE nodes SET trashed=1 WHERE id=" + quoted(kChild)));
    BOOST_REQUIRE(insertNode(*connection, kOther, "descendant", false, false, kChild));
}

BOOST_AUTO_TEST_CASE(schema_types_quota_and_blob_constraints_reject_invalid_data) {
    Database database;
    auto connection = database.current();
    seed(*connection);
    errorIs(connection->execute("UPDATE users SET id='bad'"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE users SET enabled=2"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE users SET auth_epoch=9223372036854775807+1"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE blobs SET sha256=zeroblob(31)"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE blobs SET size=-1"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE nodes SET revision=0"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE nodes SET blob_id=" + quoted(kBlob)), ErrorCode::IntegrityError);
    BOOST_REQUIRE(connection->execute("INSERT INTO space_quotas VALUES (" + quoted(kSpace) +
        ", 9223372036854775807, 10, 1099511627776, 1, 0, 9, 1)"));
    errorIs(connection->execute("UPDATE space_quotas SET used_nodes=2"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE space_quotas SET used_bytes=9223372036854775807, reserved_bytes=1"), ErrorCode::IntegrityError);
    BOOST_REQUIRE(connection->execute("UPDATE space_quotas SET used_bytes=9223372036854775807"));
    errorIs(connection->execute("UPDATE space_quotas SET used_bytes=used_bytes+1"), ErrorCode::IntegrityError);
    BOOST_TEST(scalar(*connection, "SELECT used_bytes FROM space_quotas") == std::numeric_limits<std::int64_t>::max());
}

BOOST_AUTO_TEST_CASE(independent_writers_contend_and_destructor_rolls_back) {
    Database database;
    auto first = database.current();
    auto secondResult = openSqliteDatabase(database.path(), { 0 });
    BOOST_REQUIRE(secondResult);
    auto second = std::move(secondResult).value();
    BOOST_REQUIRE(first->beginWrite());
    errorIs(first->beginWrite(), ErrorCode::InvalidArgument);
    errorIs(second->beginWrite(), ErrorCode::Retryable);
    BOOST_REQUIRE(first->execute("INSERT INTO users VALUES (" + quoted(kUser) + ", 0, 1, 0, 'member', 'Member', '" + std::string(kHash) + "', 1, 1, 0, 0)"));
    BOOST_TEST(scalar(*second, "SELECT count(*) FROM users") == 0);
    first.reset();
    BOOST_REQUIRE(second->beginWrite());
    BOOST_TEST(scalar(*second, "SELECT count(*) FROM users") == 0);
    BOOST_REQUIRE(second->rollback());
    errorIs(second->commit(), ErrorCode::InvalidArgument);
}

BOOST_AUTO_TEST_CASE(real_database_full_maps_to_storage_full_without_large_fixture) {
    Database database;
    auto connection = database.raw();
    BOOST_REQUIRE(connection->execute("CREATE TABLE payload(bytes BLOB)"));
    const auto pages = scalar(*connection, "PRAGMA page_count");
    BOOST_REQUIRE(connection->execute("PRAGMA max_page_count=" + std::to_string(pages + 2)));
    BOOST_REQUIRE(connection->beginWrite());
    // 限制数据库页数制造真实 SQLITE_FULL，内存和文件均为 KiB 级。
    errorIs(connection->execute("INSERT INTO payload VALUES (zeroblob(65536))"), ErrorCode::StorageFull);
    BOOST_TEST(connection->diagnostic().extendedCode == SQLITE_FULL);
    BOOST_REQUIRE(connection->rollback());
    BOOST_TEST(scalar(*connection, "SELECT count(*) FROM payload") == 0);
    BOOST_CHECK(std::filesystem::file_size(database.path()) < 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(commit_unknown_and_rollback_failure_do_not_allow_reuse) {
    Database database;
    auto connection = database.raw();
    BOOST_REQUIRE(connection->beginWrite());
    auto denyEnd = [](void*, int operation, const char* first, const char*, const char*, const char*) -> int {
        return operation == SQLITE_TRANSACTION && first && std::string_view(first) != "BEGIN" ? SQLITE_DENY : SQLITE_OK;
    };
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(connection->handle(), denyEnd, nullptr), SQLITE_OK);
    errorIs(connection->commit(), ErrorCode::CommitOutcomeUnknown);
    BOOST_CHECK(!connection->usable());
    errorIs(connection->beginWrite(), ErrorCode::InternalError);
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(connection->handle(), nullptr, nullptr), SQLITE_OK);
    BOOST_REQUIRE(connection->rollback());
    // 此项注入只验证保守错误分类，不模拟磁盘掉电或已经落盘的提交。
    BOOST_CHECK(sqliteError(SQLITE_BUSY, SqliteOperation::Commit, false).code == ErrorCode::CommitOutcomeUnknown);
    BOOST_CHECK(sqliteError(SQLITE_FULL, SqliteOperation::Commit, false).code == ErrorCode::CommitOutcomeUnknown);
    BOOST_CHECK(sqliteError(SQLITE_IOERR, SqliteOperation::Step).code == ErrorCode::InternalError);
}

BOOST_AUTO_TEST_CASE(schema_verification_detects_foreign_key_corruption) {
    Database database;
    auto connection = database.current();
    BOOST_REQUIRE(connection->execute("PRAGMA foreign_keys=OFF"));
    BOOST_REQUIRE(connection->execute("INSERT INTO sessions VALUES (zeroblob(32), " + quoted(kUser) + ", 0, 1, 0, 0, 'bearer')"));
    BOOST_REQUIRE(connection->execute("PRAGMA foreign_keys=ON"));
    errorIs(verifySqliteSchema(*connection), ErrorCode::IntegrityError);
}

BOOST_AUTO_TEST_CASE(schema2_identity_values_reject_invalid_encodings_and_ranges) {
    Database database;
    auto connection = database.current();
    seed(*connection);
    BOOST_TEST(scalar(*connection, "SELECT version FROM schema_info") == 3);
    BOOST_TEST(scalar(*connection, "SELECT count(*) FROM instance_settings") == 0);
    // 未引导库仅供 schema 层测试；没有管理员空间完整性的生产通过结论。
    for (const char* change : {
             "display_name=''", "display_name=char(0)", "display_name=char(31)", "display_name=char(127)",
             "display_name=char(159)", "display_name=CAST(x'ff' AS TEXT)", "display_name=zeroblob(10)",
             "password_hash='plaintext'", "password_hash=''", "password_hash=zeroblob(32)",
             "password_version=0", "revision=0", "password_version=1.5", "enabled=0", "auth_epoch=-1" }) {
        errorIs(connection->execute(std::string("UPDATE users SET ") + change), ErrorCode::IntegrityError);
    }
    BOOST_REQUIRE(connection->execute("UPDATE users SET display_name='家庭用户😀'"));
    auto insert = [&](std::string_view login, std::string_view hash) {
        auto s = prepare(*connection, "INSERT INTO users VALUES(?1,0,1,0,?2,'Member',?3,1,1,0,0)");
        BOOST_REQUIRE(s->bindText(1, kOther));
        BOOST_REQUIRE(s->bindText(2, login));
        BOOST_REQUIRE(s->bindText(3, hash));
        return s->step();
    };
    for (const auto& login : { std::string("Admin"), std::string("ab"), std::string("1abc"), std::string("abc "), std::string("a.b"), std::string("a\0bc",4), std::string(33,'a'), std::string("admin") }) {
        errorIs(insert(login, kHash), ErrorCode::IntegrityError);
    }
    for (const char* parameter : { "m=262145,t=1,p=1", "m=8192,t=11,p=1", "m=8192,t=1,p=2", "m=7,t=1,p=1" }) {
        const auto hash = std::string("$argon2id$v=19$") + parameter + "$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        errorIs(insert("member", hash), ErrorCode::IntegrityError);
    }
    BOOST_REQUIRE(insert("member", kHash));
    errorIs(connection->execute("UPDATE users SET login='renamed'"), ErrorCode::IntegrityError);
    errorIs(connection->execute("DELETE FROM users"), ErrorCode::IntegrityError);
    BOOST_REQUIRE(verifySqliteSchema(*connection));
}

BOOST_AUTO_TEST_CASE(schema2_sessions_enforce_bearer_and_temporal_shape) {
    Database database;
    auto connection = database.current();
    seed(*connection);
    const auto sql = "INSERT INTO sessions VALUES(zeroblob(32)," + quoted(kUser) + ",0,10,0,0,'bearer')";
    BOOST_REQUIRE(connection->execute(sql));
    for (const char* change : { "credential_mode='cookie'", "credential_mode=''", "created_at=10", "expires_at=-1", "revoked=2", "auth_epoch=-1", "token_hash=zeroblob(31)", "created_at=0.5" }) {
        errorIs(connection->execute(std::string("UPDATE sessions SET ") + change), ErrorCode::IntegrityError);
    }
    errorIs(connection->execute(sql), ErrorCode::IntegrityError);
    BOOST_REQUIRE(verifySqliteSchema(*connection));
}

BOOST_AUTO_TEST_CASE(schema2_invites_enforce_administrator_uniqueness_and_usage) {
    Database database;
    auto connection = database.current();
    seed(*connection);
    const auto sql = "INSERT INTO invites VALUES(" + quoted(kOther) + ",zeroblob(32)," + quoted(kUser) + ",0,2592000000000,2,0,NULL)";
    BOOST_REQUIRE(connection->execute(sql));
    for (const char* change : { "used_count=3", "used_count=-1", "used_count=0.5", "max_uses=0", "max_uses=10001", "revoked_at=0.5", "expires_at=2592000000001", "created_at=1" }) {
        errorIs(connection->execute(std::string("UPDATE invites SET ") + change), ErrorCode::IntegrityError);
    }
    BOOST_REQUIRE(connection->execute("UPDATE invites SET used_count=2,revoked_at=5"));
    errorIs(connection->execute("UPDATE invites SET used_count=1"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE invites SET revoked_at=NULL"), ErrorCode::IntegrityError);
    errorIs(connection->execute("INSERT INTO invites SELECT " + quoted(kChild) + ",token_hash,created_by,created_at,expires_at,max_uses,0,NULL FROM invites"), ErrorCode::IntegrityError);
    BOOST_REQUIRE(connection->execute("INSERT INTO users VALUES(" + quoted(kChild) + ",0,1,0,'member','Member','" + std::string(kHash) + "',1,1,0,0)"));
    errorIs(connection->execute("INSERT INTO invites VALUES(" + quoted(kBlob) + ",randomblob(32)," + quoted(kChild) + ",0,1,1,0,NULL)"), ErrorCode::IntegrityError);
    errorIs(connection->execute("INSERT INTO invites VALUES(" + quoted(kBlob) + ",randomblob(32)," + quoted(kUser) + ",0,2592000000001,1,0,NULL)"), ErrorCode::IntegrityError);
    BOOST_REQUIRE(verifySqliteSchema(*connection));
}

BOOST_AUTO_TEST_CASE(schema2_settings_and_identity_audit_have_typed_shapes) {
    Database database;
    auto connection = database.current();
    seed(*connection);
    const auto settings = "INSERT INTO instance_settings VALUES(1," + quoted(kOther) + "," + quoted(kChild) + ",0,10,0,1,0,1,1)";
    BOOST_REQUIRE(connection->execute(settings));
    errorIs(connection->execute(settings), ErrorCode::IntegrityError);
    for (const char* change : { "max_users=0", "max_users=10001", "default_private_nodes=0", "default_contribution_nodes=0", "default_private_bytes=-1", "revision=0", "instance_id='bad'" }) {
        errorIs(connection->execute(std::string("UPDATE instance_settings SET ") + change), ErrorCode::IntegrityError);
    }
    BOOST_REQUIRE(connection->execute("INSERT INTO identity_audit_events(subject_user_id,instance_id,request_id,action,occurred_at) VALUES(" + quoted(kUser) + "," + quoted(kOther) + "," + quoted(kChild) + ",'instance_initialized',0)"));
    errorIs(connection->execute("INSERT INTO identity_audit_events(request_id,action,occurred_at) VALUES(" + quoted(kChild) + ",'session_created',0)"), ErrorCode::IntegrityError);
    BOOST_REQUIRE(connection->execute("INSERT INTO identity_audit_events(actor_id,subject_user_id,request_id,action,occurred_at) VALUES(" + quoted(kUser) + "," + quoted(kUser) + "," + quoted(kChild) + ",'session_created',0)"));
    errorIs(connection->execute("UPDATE identity_audit_events SET action='arbitrary'"), ErrorCode::IntegrityError);
    errorIs(connection->execute("UPDATE identity_audit_events SET invite_id=" + quoted(kOther)), ErrorCode::IntegrityError);
    BOOST_TEST(scalar(*connection, "SELECT count(*) FROM identity_audit_events") == 2);
    BOOST_REQUIRE(verifySqliteSchema(*connection));
}

BOOST_AUTO_TEST_CASE(schema_verification_compares_real_definitions_not_just_names) {
    for (const char* change : {
             "DROP INDEX nodes_active_name; CREATE INDEX nodes_active_name ON nodes(name)",
             "DROP TRIGGER nodes_root_delete; CREATE TRIGGER nodes_root_delete BEFORE DELETE ON nodes BEGIN SELECT 1; END",
             "ALTER TABLE users ADD COLUMN hidden TEXT",
             "CREATE TABLE unexpected(value)",
             "PRAGMA writable_schema=ON; UPDATE sqlite_schema SET sql=replace(sql,'max_users BETWEEN 1 AND 10000','max_users BETWEEN 0 AND 10000') WHERE name='instance_settings'; PRAGMA writable_schema=OFF" }) {
        Database database;
        auto connection = database.current();
        BOOST_REQUIRE(connection->execute(change));
        errorIs(verifySqliteSchema(*connection), ErrorCode::IntegrityError);
        errorIs(createSqliteSchema(*connection), ErrorCode::IntegrityError);
        BOOST_CHECK(!connection->inTransaction());
    }
}

BOOST_AUTO_TEST_CASE(full_integrity_check_detects_bypassed_check_constraint) {
    Database database;
    auto connection = database.current();
    seed(*connection);
    BOOST_REQUIRE(connection->execute("PRAGMA ignore_check_constraints=ON; UPDATE users SET password_version=0; PRAGMA ignore_check_constraints=OFF"));
    errorIs(verifySqliteSchema(*connection), ErrorCode::IntegrityError);
    BOOST_TEST(scalar(*connection, "SELECT password_version FROM users") == 0);
    BOOST_CHECK(!connection->inTransaction());
    BOOST_REQUIRE(connection->beginWrite());
    BOOST_REQUIRE(connection->rollback());
}

BOOST_AUTO_TEST_CASE(verify_only_rejects_legacy_without_changing_file_or_journal_mode) {
    Database database;
    sqlite3* raw = nullptr;
    BOOST_REQUIRE_EQUAL(sqlite3_open(database.path().c_str(), &raw), SQLITE_OK);
    const int created = sqlite3_exec(raw, "CREATE TABLE schema_migrations(version INTEGER); INSERT INTO schema_migrations VALUES(1)", nullptr,nullptr,nullptr);
    const int closed = sqlite3_close(raw);
    BOOST_REQUIRE_EQUAL(created, SQLITE_OK);
    BOOST_REQUIRE_EQUAL(closed, SQLITE_OK);
    auto bytes = [&] {
        std::ifstream file(database.path(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const auto before = bytes();
    errorIs(openSqliteDatabase(database.path()), ErrorCode::InternalError);
    errorIs(createSqliteDatabase(database.path()), ErrorCode::Conflict);
    BOOST_CHECK(bytes() == before);
    BOOST_CHECK(!std::filesystem::exists(database.path().string() + "-wal"));
    BOOST_CHECK(!std::filesystem::exists(database.path().string() + "-journal"));
    // 已在 WAL 的旧布局同样拒绝；不把版本记录视作升级入口。
    auto connection = SqliteConnection::open(database.path());
    BOOST_REQUIRE(connection);
    errorIs(verifySqliteSchema(*connection.value()), ErrorCode::IntegrityError);
    errorIs(createSqliteSchema(*connection.value()), ErrorCode::IntegrityError);
    BOOST_TEST(scalar(*connection.value(), "SELECT version FROM schema_migrations") == 1);
}

BOOST_AUTO_TEST_CASE(exclusive_creation_rejects_even_empty_existing_files) {
    Database database;
    { std::ofstream file(database.path()); }
    errorIs(createSqliteDatabase(database.path()), ErrorCode::Conflict);
    BOOST_TEST(std::filesystem::file_size(database.path()) == 0);
    auto missing = database.path().parent_path() / "absent" / "db";
    errorIs(createSqliteDatabase(missing), ErrorCode::NotFound);
    BOOST_CHECK(!std::filesystem::exists(missing.parent_path()));
    std::filesystem::remove(database.path());
    for (const char* suffix : { "-wal", "-shm", "-journal" }) {
        const auto sidecar = database.path().string() + suffix;
        { std::ofstream file(sidecar); file << "preserve"; }
        errorIs(createSqliteDatabase(database.path()), ErrorCode::Conflict);
        BOOST_CHECK(!std::filesystem::exists(database.path()));
        BOOST_TEST(std::filesystem::file_size(sidecar) == 8);
        std::filesystem::remove(sidecar);
    }
}

}  // namespace anof::infrastructure::sqlite
