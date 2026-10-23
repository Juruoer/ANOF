#pragma once
#include <anof/application/ports/DriveAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteDriveAuditRepository final : public DriveAuditRepository {
public:
    explicit SqliteDriveAuditRepository(SqliteTransactionContext& c) : context_(c) {}
    Result<void> append(const DriveEvent&) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
