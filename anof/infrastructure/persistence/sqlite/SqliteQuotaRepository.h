#pragma once

#include <anof/application/ports/QuotaRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {

class SqliteQuotaRepository final : public QuotaRepository {
public:
    explicit SqliteQuotaRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<QuotaSnapshot> get(const QuotaScope&) override;
    Result<void> insert(const QuotaScope&, const QuotaSnapshot&) override;
    Result<QuotaSnapshot> setLimit(const QuotaScope&, Revision expected, QuotaDelta) override;
    Result<void> chargeUsed(const QuotaScope& scope, QuotaDelta delta) override;
private:
    Result<void> update(const QuotaScope&, const QuotaSnapshot&);
    SqliteTransactionContext& context_;
};

}  // namespace anof::infrastructure::sqlite
