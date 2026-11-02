#pragma once
#include <anof/application/ports/OfflineInstanceAccess.h>
namespace anof::infrastructure::system {
Result<std::unique_ptr<OfflineInstanceAccess>> makeLinuxOfflineInstanceAccess();
}
