#pragma once

#include <anof/application/ports/MetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteOptions.h>

#include <anof/domain/identity/Secrets.h>
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

}  // namespace anof::infrastructure::sqlite
