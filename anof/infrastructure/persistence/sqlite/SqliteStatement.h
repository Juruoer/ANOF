#pragma once

#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3_stmt;

namespace anof::infrastructure::sqlite {

class SqliteStatement {
public:
    static Result<std::unique_ptr<SqliteStatement>> prepare(SqliteConnection& connection, std::string_view sql);
    ~SqliteStatement();
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    Result<void> bindInt64(int index, std::int64_t value);
    Result<void> bindText(int index, std::string_view value);
    Result<void> bindBlob(int index, const void* data, std::size_t size);
    Result<void> bindNull(int index);
    // true 表示当前行；false 表示执行结束。完成或失败后不得再次 step。
    Result<bool> step();
    Result<std::int64_t> integer(int column) const;
    Result<std::string> text(int column) const;
    // 仅 SQLite 内部即时解码；视图到下次 step/finalize 为止，不得返回业务调用方。
    Result<std::string_view> boundedTextView(int column, std::size_t limit) const;
    Result<std::vector<std::uint8_t>> blob(int column) const;
    Result<bool> isNull(int column) const;

private:
    SqliteStatement(SqliteConnection& connection, sqlite3_stmt* statement) : connection_(connection), statement_(statement) {}
    bool validColumn(int column) const;
    Result<void> bound(int code);
    SqliteConnection& connection_;
    sqlite3_stmt* statement_;
    bool started_ = false;
    bool row_ = false;
    bool finished_ = false;
};

}  // namespace anof::infrastructure::sqlite
