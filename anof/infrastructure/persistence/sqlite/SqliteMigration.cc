#include <anof/infrastructure/persistence/sqlite/SqliteMigration.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>

#include "SqliteMigrationSql.h"

#include <sqlite3.h>

#include <chrono>

namespace anof::infrastructure::sqlite {
namespace {

// 覆盖迁移中途返回和异常退出；与业务 MetadataTransaction 无关。
class RollbackGuard {
public:
    explicit RollbackGuard(SqliteConnection& connection) : connection_(connection) {}
    ~RollbackGuard() { (void)connection_.rollback(); }
    RollbackGuard(const RollbackGuard&) = delete;
    RollbackGuard& operator=(const RollbackGuard&) = delete;
private:
    SqliteConnection& connection_;
};

Result<std::int64_t> count(SqliteConnection& connection, std::string_view sql) {
    auto statement = SqliteStatement::prepare(connection, sql);
    if (!statement) return statement.error();
    auto row = statement.value()->step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::IntegrityError;
    return statement.value()->integer(0);
}

Result<void> verifyObjects(SqliteConnection& connection) {
    // 名单同时包含约束触发器，不能只检查表存在就接受缺失的唯一索引。
    const std::pair<const char*, const char*> objects[] = {
        { "table", "schema_migrations" }, { "table", "users" }, { "table", "sessions" },
        { "table", "spaces" }, { "table", "blobs" }, { "table", "nodes" },
        { "table", "space_quotas" }, { "table", "public_contribution_quotas" }, { "table", "audit_events" },
        { "index", "users_one_administrator" }, { "index", "sessions_user" },
        { "index", "spaces_private_owner" }, { "index", "spaces_one_public" },
        { "index", "nodes_active_name" }, { "index", "nodes_active_root" }, { "index", "nodes_blob" },
        { "trigger", "nodes_shape_insert" }, { "trigger", "nodes_shape_update" },
        { "trigger", "nodes_identity_update" }, { "trigger", "nodes_root_delete" },
        { "trigger", "spaces_identity_update" }, { "trigger", "spaces_root_insert" },
        { "trigger", "public_contribution_quotas_insert" }, { "trigger", "public_contribution_quotas_update" }
    };
    for (const auto& object : objects) {
        auto statement = SqliteStatement::prepare(connection, "SELECT count(*) FROM sqlite_schema WHERE type=?1 AND name=?2");
        if (!statement) return statement.error();
        auto bound = statement.value()->bindText(1, object.first);
        if (!bound) return bound.error();
        bound = statement.value()->bindText(2, object.second);
        if (!bound) return bound.error();
        auto row = statement.value()->step();
        if (!row) return row.error();
        if (!row.value()) return ErrorCode::IntegrityError;
        auto found = statement.value()->integer(0);
        if (!found) return found.error();
        if (found.value() != 1) return ErrorCode::IntegrityError;
    }
    auto foreignKeys = SqliteStatement::prepare(connection, "PRAGMA foreign_key_check");
    if (!foreignKeys) return foreignKeys.error();
    auto row = foreignKeys.value()->step();
    if (!row) return row.error();
    if (row.value()) return ErrorCode::IntegrityError;
    return {};
}

Result<void> verifyHistory(SqliteConnection& connection) {
    auto exists = count(connection, "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='schema_migrations'");
    if (!exists) return exists.error();
    if (exists.value() != 1) return ErrorCode::InternalError;
    auto statement = SqliteStatement::prepare(connection, "SELECT version, name, definition, applied_at FROM schema_migrations ORDER BY version");
    if (!statement) {
        // 已存在的历史表若缺少必需列属于损坏；锁竞争等错误仍保留原分类。
        if ((connection.diagnostic().extendedCode & 0xff) == SQLITE_ERROR) return ErrorCode::IntegrityError;
        return statement.error();
    }
    auto row = statement.value()->step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::IntegrityError;
    auto version = statement.value()->integer(0);
    auto name = statement.value()->text(1);
    auto definition = statement.value()->text(2);
    auto timestamp = statement.value()->integer(3);
    if (!version || !name || !definition || !timestamp) return ErrorCode::IntegrityError;
    if (version.value() > 1) return ErrorCode::InternalError;
    if (version.value() != 1 || name.value() != kMigrationName || definition.value() != kMigrationSql) return ErrorCode::IntegrityError;
    row = statement.value()->step();
    if (!row) return row.error();
    if (row.value()) {
        auto extraVersion = statement.value()->integer(0);
        if (!extraVersion) return ErrorCode::IntegrityError;
        return extraVersion.value() > 1 ? ErrorCode::InternalError : ErrorCode::IntegrityError;
    }
    return verifyObjects(connection);
}

}  // namespace

Result<void> verifySqliteSchema(SqliteConnection& connection) {
    if (connection.inTransaction()) return ErrorCode::InvalidArgument;
    auto settings = connection.verifySettings();
    if (!settings) return settings.error();
    auto begun = connection.execute("BEGIN");
    if (!begun) return begun.error();
    RollbackGuard guard(connection);
    // 历史和约束目录在同一读快照中核对，结束时不保留读事务。
    auto result = verifyHistory(connection);
    if (!result) return result.error();
    return connection.rollback();
}

Result<std::unique_ptr<SqliteConnection>> openSqliteDatabase(const std::filesystem::path& path, SqliteOptions options) {
    auto connection = SqliteConnection::open(path, options);
    if (!connection) return connection.error();
    auto verified = verifySqliteSchema(*connection.value());
    if (!verified) return verified.error();
    return std::move(connection).value();
}

Result<void> migrateSqliteDatabase(const std::filesystem::path& path, SqliteOptions options) {
    auto opened = SqliteConnection::open(path, options, SqliteOpenMode::Create);
    if (!opened) return opened.error();
    return applySqliteMigrations(*opened.value());
}

Result<void> applySqliteMigrations(SqliteConnection& connection) {
    auto begun = connection.beginWrite();
    if (!begun) return begun.error();
    RollbackGuard guard(connection);
    auto objects = count(connection, "SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'");
    if (!objects) return objects.error();
    if (objects.value() != 0) {
        auto verified = verifyHistory(connection);
        if (!verified) return verified.error();
        // 已是当前版本时只验证，不写入业务数据或迁移历史。
        return connection.rollback();
    }
    auto migrated = connection.execute(kMigrationSql);
    if (!migrated) return migrated.error();
    {
        auto record = SqliteStatement::prepare(connection,
            "INSERT INTO schema_migrations(version, name, definition, applied_at) VALUES (1, ?1, ?2, ?3)");
        if (!record) return record.error();
        auto bound = record.value()->bindText(1, kMigrationName);
        if (!bound) return bound.error();
        bound = record.value()->bindText(2, kMigrationSql);
        if (!bound) return bound.error();
        const auto now = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
        bound = record.value()->bindInt64(3, now.time_since_epoch().count());
        if (!bound) return bound.error();
        auto row = record.value()->step();
        if (!row) return row.error();
        if (row.value()) return ErrorCode::IntegrityError;
    }
    auto verified = verifyHistory(connection);
    if (!verified) return verified.error();
    return connection.commit();
}

}  // namespace anof::infrastructure::sqlite
