#pragma once

#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>

namespace anof::infrastructure::sqlite {

// 维护入口；调用方须保证没有并行业务连接，生产实例锁由 runtime 负责。
Result<void> migrateSqliteDatabase(const std::filesystem::path& path, SqliteOptions options = {});
// 普通后端连接使用此入口：只验证已有版本，不创建或升级数据库。
Result<std::unique_ptr<SqliteConnection>> openSqliteDatabase(const std::filesystem::path& path, SqliteOptions options = {});
// 内部维护接口，便于在同一连接上执行迁移；拒绝已有事务。
Result<void> applySqliteMigrations(SqliteConnection& connection);
// 内部检查接口；不在应用端口暴露 SQLite 维护细节。
Result<void> verifySqliteSchema(SqliteConnection& connection);

}  // namespace anof::infrastructure::sqlite
