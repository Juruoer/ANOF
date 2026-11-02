#pragma once
#include <anof/application/ports/MetadataRepair.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/internal/MaintenanceCodec.h>
namespace anof::infrastructure::sqlite::maintenance {
MaintenanceFailure failure(const MaintenanceContext&, ErrorCode, MaintenanceStage = MaintenanceStage::Recover);
Result<std::optional<MaintenanceRun>> readRun(SqliteTransactionContext&, MaintenanceRunId);
Result<std::optional<MaintenanceActionRecord>> readAction(SqliteTransactionContext&, MaintenanceRunId, MaintenanceActionId);
Result<void> verifyRecordedAccounting(SqliteTransactionContext&);
Result<Sha256> accountingDigest(SqliteTransactionContext&, bool update);
}  // namespace anof::infrastructure::sqlite::maintenance
