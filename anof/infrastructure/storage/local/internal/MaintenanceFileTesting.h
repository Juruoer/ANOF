#pragma once
#include <anof/infrastructure/storage/local/LocalMaintenanceFileStore.h>
#include <anof/infrastructure/system/internal/MaintenanceIo.h>
namespace anof::infrastructure::storage::detail {
Result<std::unique_ptr<MaintenanceFileStore>> makeLocalMaintenanceFileStore(std::shared_ptr<system::maintenance::Io>);
}
