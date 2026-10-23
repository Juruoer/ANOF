#pragma once

#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>

#include <cstdint>

namespace anof::infrastructure::sqlite {

inline constexpr std::int64_t kSqliteSchemaVersion = 4;
// 仅限 SQLite 内部：在无活动事务且没有用户对象的库上原子建立当前 schema。
// 已有当前库只校验；旧/未知/损坏布局拒绝，不升级或重置。
Result<void> createSqliteSchema(SqliteConnection& connection);
// 只接纳不存在的文件；独占创建 0600 主库，失败保留现场，不删除文件。
// 实例路径和跨进程 lease 校验属于初始化适配器。
Result<void> createSqliteDatabase(const std::filesystem::path& path, SqliteOptions options = {});
// 当前 schema 层校验；成功不证明 settings/管理员/根/额度已经完整引导。
Result<void> verifySqliteSchema(SqliteConnection& connection);
Result<std::unique_ptr<SqliteConnection>> openSqliteDatabase(const std::filesystem::path& path, SqliteOptions options = {});

}  // namespace anof::infrastructure::sqlite
