#pragma once

#include <anof/application/ports/AuthorizationRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {

class SqliteAuthorizationRepository final : public AuthorizationRepository {
public:
    explicit SqliteAuthorizationRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<Principal> requireActiveSession(const SessionDigest& digest, UtcTime now) override;
private:
    SqliteTransactionContext& context_;
};

}  // namespace anof::infrastructure::sqlite
