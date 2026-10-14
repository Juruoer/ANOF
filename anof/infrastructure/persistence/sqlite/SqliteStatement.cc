#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>

#include <sqlite3.h>

#include <limits>

namespace anof::infrastructure::sqlite {

Result<std::unique_ptr<SqliteStatement>> SqliteStatement::prepare(SqliteConnection& connection, std::string_view sql) {
    if (!connection.usable()) return ErrorCode::InternalError;
    if (sql.empty() || sql.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()) || sql.find('\0') != sql.npos) {
        return ErrorCode::InvalidArgument;
    }
    sqlite3_stmt* statement = nullptr;
    const char* tail = nullptr;
    const int code = sqlite3_prepare_v2(connection.handle(), sql.data(), static_cast<int>(sql.size()), &statement, &tail);
    if (code != SQLITE_OK) {
        sqlite3_finalize(statement);
        return connection.recordError(code, SqliteOperation::Prepare);
    }
    // 不允许无意中只执行多条 SQL 的第一条；建库脚本由 execute 专门处理。
    const auto consumed = static_cast<std::size_t>(tail - sql.data());
    if (!statement || sql.substr(consumed).find_first_not_of(" \t\r\n") != sql.npos) {
        sqlite3_finalize(statement);
        return ErrorCode::InvalidArgument;
    }
    return std::unique_ptr<SqliteStatement>(new SqliteStatement(connection, statement));
}

SqliteStatement::~SqliteStatement() { sqlite3_finalize(statement_); }

Result<void> SqliteStatement::bound(int code) {
    if (code != SQLITE_OK) return connection_.recordError(code, SqliteOperation::Bind);
    return {};
}

Result<void> SqliteStatement::bindInt64(int index, std::int64_t value) {
    if (started_ || !connection_.usable()) return ErrorCode::InvalidArgument;
    return bound(sqlite3_bind_int64(statement_, index, value));
}

Result<void> SqliteStatement::bindText(int index, std::string_view value) {
    if (started_ || !connection_.usable()) return ErrorCode::InvalidArgument;
    return bound(sqlite3_bind_text64(statement_, index, value.empty() ? "" : value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
}

Result<void> SqliteStatement::bindBlob(int index, const void* data, std::size_t size) {
    if (started_ || !connection_.usable() || (!data && size != 0)) return ErrorCode::InvalidArgument;
    return bound(sqlite3_bind_blob64(statement_, index, size == 0 ? "" : data, size, SQLITE_TRANSIENT));
}

Result<void> SqliteStatement::bindNull(int index) {
    if (started_ || !connection_.usable()) return ErrorCode::InvalidArgument;
    return bound(sqlite3_bind_null(statement_, index));
}

Result<bool> SqliteStatement::step() {
    if (finished_ || !connection_.usable()) return ErrorCode::InvalidArgument;
    started_ = true;
    row_ = false;
    const int code = sqlite3_step(statement_);
    if (code == SQLITE_ROW) { row_ = true; return true; }
    finished_ = true;
    if (code == SQLITE_DONE) return false;
    return connection_.recordError(code, SqliteOperation::Step);
}

bool SqliteStatement::validColumn(int column) const {
    return row_ && connection_.usable() && column >= 0 && column < sqlite3_column_count(statement_);
}

Result<bool> SqliteStatement::isNull(int column) const {
    if (!validColumn(column)) return ErrorCode::InvalidArgument;
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

Result<std::int64_t> SqliteStatement::integer(int column) const {
    if (!validColumn(column)) return ErrorCode::InvalidArgument;
    if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) return ErrorCode::IntegrityError;
    return static_cast<std::int64_t>(sqlite3_column_int64(statement_, column));
}

Result<std::string> SqliteStatement::text(int column) const {
    if (!validColumn(column)) return ErrorCode::InvalidArgument;
    if (sqlite3_column_type(statement_, column) != SQLITE_TEXT) return ErrorCode::IntegrityError;
    const auto* bytes = sqlite3_column_text(statement_, column);
    if (!bytes) return connection_.recordError(SQLITE_NOMEM, SqliteOperation::Step);
    return std::string(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)));
}

Result<std::string_view> SqliteStatement::boundedTextView(int column, std::size_t limit) const {
    if (!validColumn(column)) return ErrorCode::InvalidArgument;
    if (sqlite3_column_type(statement_, column) != SQLITE_TEXT) return ErrorCode::IntegrityError;
    const int size = sqlite3_column_bytes(statement_, column);
    if (size < 0 || static_cast<std::size_t>(size) > limit) return ErrorCode::IntegrityError;
    const auto* bytes = sqlite3_column_text(statement_, column);
    if (!bytes) return connection_.recordError(SQLITE_NOMEM, SqliteOperation::Step);
    return std::string_view(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(size));
}

Result<std::vector<std::uint8_t>> SqliteStatement::blob(int column) const {
    if (!validColumn(column)) return ErrorCode::InvalidArgument;
    if (sqlite3_column_type(statement_, column) != SQLITE_BLOB) return ErrorCode::IntegrityError;
    const auto* bytes = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement_, column));
    const int size = sqlite3_column_bytes(statement_, column);
    if (size == 0) return std::vector<std::uint8_t>{};
    if (!bytes) return connection_.recordError(SQLITE_NOMEM, SqliteOperation::Step);
    return std::vector<std::uint8_t>(bytes, bytes + size);
}

}  // namespace anof::infrastructure::sqlite
