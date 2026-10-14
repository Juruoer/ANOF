#pragma once

#include <anof/application/ports/MetadataReadSnapshot.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
#include <memory>

namespace anof::infrastructure::sqlite {
// 内部构造入口，返回对象借用 store 连接和活动标记。
Result<std::unique_ptr<MetadataReadSnapshot>> makeSqliteReadSnapshot(
    SqliteConnection&, bool& storeActive, PasswordHashLimits);
}  // namespace anof::infrastructure::sqlite
