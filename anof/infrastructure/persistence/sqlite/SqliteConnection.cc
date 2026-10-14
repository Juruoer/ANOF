#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>

#include <anof/infrastructure/persistence/sqlite/SqliteFunctions.h>

#include <sqlite3.h>

#include <string>

namespace anof::infrastructure::sqlite {
namespace {

Result<void> checkInteger(SqliteConnection& connection, std::string_view sql, std::int64_t expected) {
    auto prepared = SqliteStatement::prepare(connection, sql);
    if (!prepared) return prepared.error();
    auto& statement = *prepared.value();
    auto row = statement.step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::InternalError;
    auto value = statement.integer(0);
    if (!value) return value.error();
    if (value.value() != expected) return ErrorCode::InternalError;
    row = statement.step();
    if (!row) return row.error();
    if (row.value()) return ErrorCode::InternalError;
    return {};
}

}  // namespace

Result<std::unique_ptr<SqliteConnection>> SqliteConnection::open(
    const std::filesystem::path& path, SqliteOptions options, SqliteOpenMode mode) {
    const auto filename = path.string();
    if (filename.empty() || filename == ":memory:" || filename.find('\0') != filename.npos ||
        options.busyTimeoutMs < 0 || options.busyTimeoutMs > 5000) return ErrorCode::InvalidArgument;
    sqlite3* handle = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE |
                      (mode == SqliteOpenMode::Create ? SQLITE_OPEN_CREATE : 0);
    const int code = sqlite3_open_v2(filename.c_str(), &handle, flags, nullptr);
    if (code != SQLITE_OK) {
        const int extended = handle ? sqlite3_extended_errcode(handle) : code;
        sqlite3_close_v2(handle);
        return sqliteError(extended, SqliteOperation::Open);
    }
    auto connection = std::unique_ptr<SqliteConnection>(new SqliteConnection(handle));
    auto configured = connection->configure(options, mode);
    if (!configured) return configured.error();
    return connection;
}

SqliteConnection::~SqliteConnection() {
    // close_v2 对异常路径仍保证释放；调用方必须先销毁所有语句。
    if (inTransaction()) (void)rollback();
    sqlite3_close_v2(handle_);
}

Error SqliteConnection::recordError(int code, SqliteOperation operation) {
    diagnostic_ = { operation, code };
    return sqliteError(code, operation);
}

Result<void> SqliteConnection::configure(SqliteOptions options, SqliteOpenMode mode) {
    int code = sqlite3_extended_result_codes(handle_, 1);
    if (code != SQLITE_OK) return recordError(code, SqliteOperation::Configure);
    code = sqlite3_busy_timeout(handle_, options.busyTimeoutMs);
    if (code != SQLITE_OK) return recordError(code, SqliteOperation::Configure);
    auto functions = registerSqliteFunctions(handle_);
    if (!functions) return functions.error();
    auto configured = execute(mode == SqliteOpenMode::Inspect
        ? "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;"
        : "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
    if (!configured) return configured.error();
    return verifySettings();
}

Result<void> SqliteConnection::verifySettings() {
    if (!usable_) return ErrorCode::InternalError;
    auto checked = checkInteger(*this, "PRAGMA foreign_keys", 1);
    if (!checked) return checked.error();
    checked = checkInteger(*this, "PRAGMA synchronous", 2);
    if (!checked) return checked.error();
    auto prepared = SqliteStatement::prepare(*this, "PRAGMA journal_mode");
    if (!prepared) return prepared.error();
    auto& statement = *prepared.value();
    auto row = statement.step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::InternalError;
    auto mode = statement.text(0);
    if (!mode) return mode.error();
    if (mode.value() != "wal") return ErrorCode::InternalError;
    return {};
}

Result<void> SqliteConnection::execute(std::string_view sql) {
    if (!usable_) return ErrorCode::InternalError;
    if (sql.find('\0') != sql.npos) return ErrorCode::InvalidArgument;
    const std::string owned(sql);
    const int code = sqlite3_exec(handle_, owned.c_str(), nullptr, nullptr, nullptr);
    if (code != SQLITE_OK) return recordError(code, SqliteOperation::Step);
    return {};
}

bool SqliteConnection::inTransaction() const noexcept { return sqlite3_get_autocommit(handle_) == 0; }

Result<void> SqliteConnection::beginWrite() {
    if (!usable_) return ErrorCode::InternalError;
    if (inTransaction()) return ErrorCode::InvalidArgument;
    auto settings = verifySettings();
    if (!settings) return settings.error();
    const int code = sqlite3_exec(handle_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    if (code != SQLITE_OK) return recordError(code, SqliteOperation::Begin);
    return {};
}

Result<void> SqliteConnection::beginRead() {
    if (!usable_) return ErrorCode::InternalError;
    if (inTransaction()) return ErrorCode::InvalidArgument;
    auto settings = verifySettings();
    if (!settings) return settings.error();
    auto restricted = execute("PRAGMA query_only=ON");
    if (!restricted) return restricted.error();
    auto begun = execute("BEGIN");
    if (!begun) {
        if (!execute("PRAGMA query_only=OFF")) usable_ = false;
        return begun.error();
    }
    return {};
}

Result<void> SqliteConnection::endRead() {
    auto ended = rollback();
    if (!ended) return ended.error();
    auto restored = execute("PRAGMA query_only=OFF");
    if (!restored) usable_ = false;
    return restored;
}

Result<void> SqliteConnection::rollback() {
    if (!inTransaction()) return {};
    const int code = sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, nullptr);
    if (code != SQLITE_OK || inTransaction()) {
        usable_ = false;
        return recordError(code == SQLITE_OK ? SQLITE_INTERNAL : code, SqliteOperation::Rollback);
    }
    return {};
}

Result<void> SqliteConnection::commit() {
    if (!usable_) return ErrorCode::InternalError;
    if (!inTransaction()) return ErrorCode::InvalidArgument;
    const int code = sqlite3_exec(handle_, "COMMIT", nullptr, nullptr, nullptr);
    if (code == SQLITE_OK) return {};
    // 失败后仍有事务且回滚成功，才能证明本次修改没有提交。
    const bool active = inTransaction();
    bool knownNotCommitted = false;
    if (active) knownNotCommitted = static_cast<bool>(rollback());
    if (!knownNotCommitted) usable_ = false;
    diagnostic_ = { SqliteOperation::Commit, code };
    return sqliteError(code, SqliteOperation::Commit, knownNotCommitted);
}

}  // namespace anof::infrastructure::sqlite
