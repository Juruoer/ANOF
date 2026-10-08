#pragma once

#include <anof/core/Result.h>
#include <anof/infrastructure/persistence/sqlite/SqliteError.h>
#include <anof/infrastructure/persistence/sqlite/SqliteOptions.h>

#include <filesystem>
#include <memory>
#include <string_view>

struct sqlite3;

namespace anof::infrastructure::sqlite {

enum class SqliteOpenMode { Existing, Create };

// SQLite 内部基础设施，不是业务端口；连接必须比借用它的语句活得久。
// 单线程使用，禁止共享到其他 worker。open 只配置连接，不执行 schema 迁移。
class SqliteConnection {
public:
    static Result<std::unique_ptr<SqliteConnection>> open(
        const std::filesystem::path& path, SqliteOptions options = {}, SqliteOpenMode mode = SqliteOpenMode::Existing);
    ~SqliteConnection();
    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    Result<void> execute(std::string_view sql);
    Result<void> verifySettings();
    Result<void> beginWrite();
    Result<void> commit();
    Result<void> rollback();
    bool inTransaction() const noexcept;
    bool usable() const noexcept { return usable_; }
    const SqliteDiagnostic& diagnostic() const noexcept { return diagnostic_; }

    // 仅供 SQLite 内部语句与故障测试使用，不穿透 MetadataStore。
    sqlite3* handle() const noexcept { return handle_; }
    Error recordError(int code, SqliteOperation operation);

private:
    explicit SqliteConnection(sqlite3* handle) : handle_(handle) {}
    Result<void> configure(SqliteOptions options);
    sqlite3* handle_;
    bool usable_ = true;
    SqliteDiagnostic diagnostic_;
};

}  // namespace anof::infrastructure::sqlite
