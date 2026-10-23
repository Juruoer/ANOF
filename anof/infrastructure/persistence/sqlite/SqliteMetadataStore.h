#pragma once
#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/application/ports/TransferMetadataStore.h>
#include <anof/domain/identity/Secrets.h>
#include <anof/infrastructure/persistence/sqlite/SqliteOptions.h>

#include <filesystem>

namespace anof::infrastructure::sqlite {

struct SqliteMetadataOptions {
    PasswordHashLimits passwordHashLimits{};
    SqliteOptions connection;
};

// 打开已有且已引导的数据库，不创建或升级 schema。返回值仅公开业务端口。
// store 必须比事务活得久；store 和事务仅在所属元数据线程上使用。
Result<std::unique_ptr<MetadataStore>> openSqliteMetadataStore(
    const std::filesystem::path& path, SqliteMetadataOptions options);

// 与基础 opener 使用同一生产结构和已引导状态校验，不需调用方 downcast。
Result<std::unique_ptr<DriveMetadataStore>> openSqliteDriveMetadataStore(const std::filesystem::path&, SqliteMetadataOptions);
Result<std::unique_ptr<TransferMetadataStore>> openSqliteTransferMetadataStore(
    const std::filesystem::path& path, SqliteMetadataOptions options);

}  // namespace anof::infrastructure::sqlite
