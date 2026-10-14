#pragma once
#include <anof/application/maintenance/Initialization.h>
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>
namespace anof::infrastructure::sqlite::detail {
// 独立读快照；initial 要求精确初始计费，普通运行允许后续合法业务变化。
Result<InitReceipt> verifyInitialized(SqliteConnection&, PasswordHashLimits, bool initial = false);
inline bool validPasswordLimits(PasswordHashLimits limits) {
    return limits.maxMemoryKiB >= 8 && limits.maxMemoryKiB <= 262144 && limits.maxIterations >= 1 && limits.maxIterations <= 10 && limits.maxParallelism == 1;
}
}
