#pragma once
#include <anof/domain/maintenance/MaintenanceValues.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite::maintenance {
Result<Sha256> repairInvariant(SqliteTransactionContext&, const MaintenancePlan&);
}
