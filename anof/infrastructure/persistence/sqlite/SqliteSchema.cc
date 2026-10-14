#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteFunctions.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include "SqliteSchemaSql.h"

#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <memory>
#include <string>

namespace anof::infrastructure::sqlite {
namespace {
class RollbackGuard {
public:
    explicit RollbackGuard(SqliteConnection& connection) : connection_(connection) {}
    ~RollbackGuard() { (void)connection_.rollback(); }
private:
    SqliteConnection& connection_;
};
Result<std::int64_t> scalar(SqliteConnection& connection, std::string_view sql) {
    auto statement = SqliteStatement::prepare(connection, sql);
    if (!statement) return statement.error();
    auto row = statement.value()->step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::IntegrityError;
    return statement.value()->integer(0);
}

Result<void> verifyDefinition(SqliteConnection& connection) {
    // 参考库只执行编译进程序的 DDL，绝不执行数据库记录的 definition。
    sqlite3* raw = nullptr;
    const int code = sqlite3_open_v2(":memory:", &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> reference(raw, sqlite3_close_v2);
    if (code != SQLITE_OK) return sqliteError(code, SqliteOperation::Open);
    auto functions = registerSqliteFunctions(raw);
    if (!functions) return functions.error();
    int result = sqlite3_exec(raw, kSchemaSql, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) return sqliteError(result, SqliteOperation::Step);
    sqlite3_stmt* query = nullptr;
    result = sqlite3_prepare_v2(raw, "SELECT type,name,tbl_name,sql FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*' ORDER BY name", -1, &query, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> expected(query, sqlite3_finalize);
    if (result != SQLITE_OK) return sqliteError(result, SqliteOperation::Prepare);
    std::int64_t count = 0;
    while ((result = sqlite3_step(query)) == SQLITE_ROW) {
        auto match = SqliteStatement::prepare(connection,
            "SELECT count(*) FROM sqlite_schema WHERE type=?1 AND name=?2 AND tbl_name=?3 AND sql=?4");
        if (!match) return match.error();
        for (int column = 0; column != 4; ++column) {
            const auto* text = sqlite3_column_text(query, column);
            if (!text) return ErrorCode::InternalError;
            auto bound = match.value()->bindText(column + 1, std::string_view(reinterpret_cast<const char*>(text), static_cast<std::size_t>(sqlite3_column_bytes(query, column))));
            if (!bound) return bound.error();
        }
        auto row = match.value()->step();
        if (!row) return row.error();
        if (!row.value()) return ErrorCode::IntegrityError;
        auto matches = match.value()->integer(0);
        if (!matches || matches.value() != 1) return ErrorCode::IntegrityError;
        ++count;
    }
    if (result != SQLITE_DONE) return sqliteError(result, SqliteOperation::Step);
    auto actual = scalar(connection, "SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'");
    if (!actual) return actual.error();
    if (actual.value() != count) return ErrorCode::IntegrityError;
    return {};
}

Result<void> verifyInside(SqliteConnection& connection) {
    auto exists = scalar(connection, "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='schema_info'");
    if (!exists) return exists.error();
    if (exists.value() != 1) return ErrorCode::IntegrityError;
    auto definition = verifyDefinition(connection);
    if (!definition) return definition.error();
    auto record = SqliteStatement::prepare(connection, "SELECT version, definition=?1 FROM schema_info WHERE singleton=1");
    if (!record) return record.error();
    auto bound = record.value()->bindText(1, kSchemaSql);
    if (!bound) return bound.error();
    auto row = record.value()->step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::IntegrityError;
    auto version = record.value()->integer(0);
    auto matches = record.value()->integer(1);
    if (!version || !matches || version.value() != kSqliteSchemaVersion || matches.value() != 1) return ErrorCode::IntegrityError;
    auto foreignKeys = SqliteStatement::prepare(connection, "PRAGMA foreign_key_check");
    if (!foreignKeys) return foreignKeys.error();
    row = foreignKeys.value()->step();
    if (!row) return row.error();
    if (row.value()) return ErrorCode::IntegrityError;
    // 返回内容只在 SQLite 内比较，最多一条损坏诊断，不回显行内容。
    auto integrity = SqliteStatement::prepare(connection, "SELECT integrity_check='ok' FROM pragma_integrity_check(1)");
    if (!integrity) return integrity.error();
    row = integrity.value()->step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::IntegrityError;
    auto ok = integrity.value()->integer(0);
    if (!ok || ok.value() != 1) return ErrorCode::IntegrityError;
    row = integrity.value()->step();
    if (!row) return row.error();
    return row.value() ? Result<void>(ErrorCode::IntegrityError) : Result<void>();
}
}

Result<void> verifySqliteSchema(SqliteConnection& connection) {
    if (connection.inTransaction()) return ErrorCode::InvalidArgument;
    auto settings = connection.verifySettings();
    if (!settings) return settings.error();
    auto begun = connection.execute("BEGIN");
    if (!begun) return begun.error();
    RollbackGuard guard(connection);
    auto verified = verifyInside(connection);
    if (!verified) return verified.error();
    return connection.rollback();
}

Result<std::unique_ptr<SqliteConnection>> openSqliteDatabase(const std::filesystem::path& path, SqliteOptions options) {
    auto connection = SqliteConnection::open(path, options, SqliteOpenMode::Inspect);
    if (!connection) return connection.error();
    auto verified = verifySqliteSchema(*connection.value());
    if (!verified) return verified.error();
    return std::move(connection).value();
}

Result<void> createSqliteSchema(SqliteConnection& connection) {
    auto begun = connection.beginWrite();
    if (!begun) return begun.error();
    RollbackGuard guard(connection);
    auto objects = scalar(connection, "SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'");
    if (!objects) return objects.error();
    if (objects.value() != 0) {
        auto verified = verifyInside(connection);
        if (!verified) return verified.error();
        return connection.rollback();
    }
    auto created = connection.execute(kSchemaSql);
    if (!created) return created.error();
    {
        auto record = SqliteStatement::prepare(connection, "INSERT INTO schema_info VALUES(1,?1,?2,?3)");
        if (!record) return record.error();
        auto bound = record.value()->bindInt64(1, kSqliteSchemaVersion);
        if (!bound) return bound.error();
        bound = record.value()->bindText(2, kSchemaSql);
        if (!bound) return bound.error();
        const auto now = std::chrono::floor<std::chrono::microseconds>(std::chrono::system_clock::now());
        bound = record.value()->bindInt64(3, now.time_since_epoch().count());
        if (!bound) return bound.error();
        auto row = record.value()->step();
        if (!row) return row.error();
        if (row.value()) return ErrorCode::IntegrityError;
    }
    auto verified = verifyInside(connection);
    if (!verified) return verified.error();
    return connection.commit();
}

Result<void> createSqliteDatabase(const std::filesystem::path& path, SqliteOptions options) {
    const auto name = path.string();
    if (name.empty() || name == ":memory:" || name.find('\0') != name.npos || options.busyTimeoutMs < 0 || options.busyTimeoutMs > 5000) return ErrorCode::InvalidArgument;
    // 缺失主库不代表伴随文件可以被重用；保留孤立 WAL/journal 等现场。
    for (const char* suffix : { "-wal", "-shm", "-journal" }) {
        struct stat companion{};
        if (::lstat((name + suffix).c_str(), &companion) == 0) return ErrorCode::Conflict;
        if (errno != ENOENT) return ErrorCode::InternalError;
    }
    const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        if (errno == EEXIST) return ErrorCode::Conflict;
        if (errno == ENOENT) return ErrorCode::NotFound;
        if (errno == ENOSPC || errno == EDQUOT) return ErrorCode::StorageFull;
        return ErrorCode::InternalError;
    }
    // 保留 fd 到 SQLite 连接关闭后，失败保留文件，避免回滚现场被隐式删除。
    struct CloseFd { int fd; ~CloseFd() { ::close(fd); } } held{ fd };
    struct stat status{};
    if (::fstat(fd, &status) != 0) return ErrorCode::InternalError;
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 || (status.st_mode & 07777) != 0600) return ErrorCode::IntegrityError;
    auto connection = SqliteConnection::open(path, options);
    if (!connection) return connection.error();
    return createSqliteSchema(*connection.value());
}
}  // namespace anof::infrastructure::sqlite
