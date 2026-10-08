#pragma once

#include <anof/application/ports/QuotaRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {

class SqliteQuotaRepository final : public QuotaRepository {
public:
    explicit SqliteQuotaRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<void> chargeUsed(const QuotaScope& scope, QuotaDelta delta) override;
private:
    SqliteTransactionContext& context_;
};

}  // namespace anof::infrastructure::sqlite
