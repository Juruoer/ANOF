#pragma once

#include <anof/core/Result.h>
#include <anof/infrastructure/persistence/sqlite/SqliteError.h>
#include <anof/infrastructure/persistence/sqlite/SqliteOptions.h>

#include <filesystem>
#include <memory>
#include <string_view>

struct sqlite3;

namespace anof::infrastructure::sqlite {

// Inspect 不设置持久 journal_mode；其他模式只用于显式建库或内部原始连接。
enum class SqliteOpenMode { Existing,
                            Create,
                            Inspect,
                            MaintenanceCopy };

// SQLite 内部基础设施，不是业务端口；连接必须比借用它的语句活得久。
// 单线程使用，禁止共享到其他 worker。open 只配置连接，不执行建库或 schema 升级。
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
    Result<void> beginRead();
    Result<void> endRead();
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
    Result<void> configure(SqliteOptions options, SqliteOpenMode mode);
    sqlite3* handle_;
    bool usable_ = true;
    bool maintenanceCopy_ = false;
    SqliteDiagnostic diagnostic_;
};

}  // namespace anof::infrastructure::sqlite
