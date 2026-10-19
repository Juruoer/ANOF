#pragma once
#include <anof/application/ports/TransferAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteTransferAuditRepository final : public TransferAuditRepository {
public:
    explicit SqliteTransferAuditRepository(SqliteTransactionContext& c) : context_(c) {}
    Result<void> append(const TransferEvent&) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
