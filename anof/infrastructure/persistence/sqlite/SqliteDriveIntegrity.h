#pragma once
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>
namespace anof::infrastructure::sqlite {
// 不修复数据；typed事务提交与启动检查复用回收站完整成员验证。
Result<void> verifyDriveMembership(SqliteConnection&);
}  // namespace anof::infrastructure::sqlite
