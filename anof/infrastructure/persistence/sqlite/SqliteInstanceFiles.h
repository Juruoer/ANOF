#pragma once
#include <anof/application/ports/InstanceInitialization.h>
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>
namespace anof::infrastructure::sqlite::detail {
InitFailure maintenanceFailure(ErrorCode, InitStage);
Error maintenanceError(InitError);
InitResult<void> verifyInitializationFiles(const InstanceLease&, const InitializationIdentity&, bool creating);
struct InstanceDatabase {
    // 声明顺序保证 SQLite 先关闭；连接有 POSIX 锁时不能先关闭另一个主库 fd。
    system::detail::OwnedFd file;
    std::unique_ptr<SqliteConnection> connection;
    InitResult<void> close(const InstanceLease&, bool synchronize, InitStage);
};
InitResult<std::unique_ptr<InstanceDatabase>> openInstanceDatabase(const InstanceLease&, SqliteOptions, bool create);
}
