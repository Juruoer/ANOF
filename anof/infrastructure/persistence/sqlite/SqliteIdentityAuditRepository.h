#pragma once

#include <anof/application/ports/IdentityAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {
// 仓储借用所属事务；不独立打开连接或提交，引用不得超过上下文寿命。
class SqliteIdentityAuditRepository final : public IdentityAuditRepository {
public:
    explicit SqliteIdentityAuditRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<void> append(const IdentityEvent&) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
