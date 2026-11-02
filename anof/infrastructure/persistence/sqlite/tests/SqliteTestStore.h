#pragma once

#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStoreInternal.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>

namespace anof::infrastructure::sqlite::testing {
// 仅局部契约夹具：允许没有 settings 或完整根/额度的当前库，不走生产打开工厂。
inline Result<std::unique_ptr<MetadataStore>> openTestMetadataStore(const std::filesystem::path& path, SqliteOptions options = {}) {
    // reset 已创建当前 schema；故障测试可以在打开前注入损坏或触发器。
    auto connection = SqliteConnection::open(path, options, SqliteOpenMode::Inspect);
    if (!connection) return connection.error();
    return std::unique_ptr<MetadataStore>(new detail::MetadataStoreImpl(std::move(connection).value(), { 8192, 1, 1 }));
}
}  // namespace anof::infrastructure::sqlite::testing
