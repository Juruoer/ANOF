#pragma once

#include <anof/application/ports/MetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteOptions.h>

#include <filesystem>

namespace anof::infrastructure::sqlite {

// 打开已迁移的数据库，不创建或升级 schema。返回值仅公开业务端口。
// store 必须比事务活得久；store 和事务仅在所属元数据线程上使用。
Result<std::unique_ptr<MetadataStore>> openSqliteMetadataStore(
    const std::filesystem::path& path, SqliteOptions options = {});

}  // namespace anof::infrastructure::sqlite
