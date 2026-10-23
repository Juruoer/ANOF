#pragma once
#include <anof/application/ports/NodeAccountingRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteNodeAccountingRepository final : public NodeAccountingRepository {
public:
    explicit SqliteNodeAccountingRepository(SqliteTransactionContext& c) : context_(c) {}
    Result<NodeAccounting> get() override;
    Result<void> chargeUsed(NodeCount) override;
    Result<void> releaseUsed(NodeCount) override;
    Result<void> reserve(NodeCount) override;
    Result<void> commitReservation(NodeCount) override;
    Result<void> releaseReservation(NodeCount) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
