#pragma once

#include <anof/application/ports/TransferMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {

class SqliteQuotaRepository final : public ReservationQuotaRepository {
public:
    explicit SqliteQuotaRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<QuotaSnapshot> get(const QuotaScope&) override;
    Result<void> insert(const QuotaScope&, const QuotaSnapshot&) override;
    Result<QuotaSnapshot> setLimit(const QuotaScope&, Revision expected, QuotaDelta) override;
    Result<void> chargeUsed(const QuotaScope& scope, QuotaDelta delta) override;
    Result<void> reserve(const QuotaScope&, QuotaDelta) override;
    Result<void> commitReservation(const QuotaScope&, QuotaDelta) override;
    Result<void> releaseReservation(const QuotaScope&, QuotaDelta) override;

private:
    Result<void> reservation(const QuotaScope&, QuotaDelta, int);
    Result<void> update(const QuotaScope&, const QuotaSnapshot&);
    SqliteTransactionContext& context_;
};

}  // namespace anof::infrastructure::sqlite
