#pragma once
#include <anof/application/ports/MaintenanceFileStore.h>
namespace anof::infrastructure::storage {
Result<std::unique_ptr<MaintenanceFileStore>> makeLocalMaintenanceFileStore();
}
