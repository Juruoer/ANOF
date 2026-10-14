#pragma once

#include <anof/application/ports/SessionRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {
// 仓储借用所属事务；不独立打开连接或提交，引用不得超过上下文寿命。
class SqliteSessionRepository final : public SessionRepository {
public:
    explicit SqliteSessionRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<SessionCount> countActive(UserId, UtcTime) override;
    Result<void> insert(const NewSession&) override;
    Result<bool> revoke(const SessionCredential&, UserId, UtcTime) override;
    Result<SessionCount> revokeAll(UserId, UtcTime) override;
    Result<SessionCount> eraseInactive(UtcTime, PageSize) override;
private:
    SqliteTransactionContext& context_;

};
}
