#pragma once

#include <anof/application/ports/AuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {

class SqliteAuditRepository final : public AuditRepository {
public:
    explicit SqliteAuditRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<void> append(const DirectoryCreatedEvent& event) override;
private:
    SqliteTransactionContext& context_;
};

}  // namespace anof::infrastructure::sqlite
