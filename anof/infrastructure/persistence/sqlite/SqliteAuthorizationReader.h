#pragma once

#include <anof/application/ports/AuthorizationReader.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {

class SqliteAuthorizationReader final : public AuthorizationReader {
public:
    explicit SqliteAuthorizationReader(SqliteTransactionContext& context) : context_(context) {}
    Result<Principal> requireActiveSession(const SessionCredential& credential, UtcTime now) override;
private:
    SqliteTransactionContext& context_;
};

}  // namespace anof::infrastructure::sqlite
