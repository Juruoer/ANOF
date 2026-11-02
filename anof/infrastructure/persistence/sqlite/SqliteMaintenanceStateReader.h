#pragma once
#include <anof/application/ports/MaintenanceStateReader.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteMaintenanceStateReader final : public MaintenanceStateReader {
public:
    explicit SqliteMaintenanceStateReader(SqliteTransactionContext& context) : context_(context) {}
    Result<std::optional<QuarantineEntry>> findQuarantine(QuarantineId) override;
    Result<bool> hasPendingRuns() override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
