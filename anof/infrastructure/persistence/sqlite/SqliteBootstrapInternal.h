#pragma once
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceBootstrap.h>
namespace anof::infrastructure::sqlite::detail {
// 仅用于局部故障测试；生产工厂不接受回调，不改变公共维护端口。
enum class BootstrapPoint { Begun,
                            BeforeAudit,
                            BeforeCommit };
using BootstrapHook = Result<void> (*)(SqliteConnection&, BootstrapPoint);
InitResult<std::unique_ptr<InstanceBootstrap>> makeSqliteInstanceBootstrapWith(const SqliteBootstrapOptions&, BootstrapHook);
}  // namespace anof::infrastructure::sqlite::detail
