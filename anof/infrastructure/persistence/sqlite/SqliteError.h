#pragma once

#include <anof/core/Error.h>

namespace anof::infrastructure::sqlite {

enum class SqliteOperation { Open,
                             Configure,
                             Prepare,
                             Bind,
                             Step,
                             Begin,
                             Commit,
                             Rollback };

// 仅保存诊断分类，避免把 SQL、路径或绑定凭据写入公开错误。
struct SqliteDiagnostic {
    SqliteOperation operation = SqliteOperation::Open;
    int extendedCode = 0;
};

// commit 失败且不能证明未提交时，优先返回 CommitOutcomeUnknown。
Error sqliteError(int extendedCode, SqliteOperation operation, bool knownNotCommitted = true);

}  // namespace anof::infrastructure::sqlite
