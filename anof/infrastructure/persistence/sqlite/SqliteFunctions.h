#pragma once
#include <anof/core/Result.h>
struct sqlite3;
namespace anof::infrastructure::sqlite {
// schema CHECK 使用的纯、有界验证函数；不访问库、不执行密码算法。
Result<void> registerSqliteFunctions(sqlite3* handle);
}  // namespace anof::infrastructure::sqlite
