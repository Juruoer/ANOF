#pragma once

#include <anof/application/ports/InviteRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {
// 仓储借用所属事务；不独立打开连接或提交，引用不得超过上下文寿命。
class SqliteInviteRepository final : public InviteRepository {
public:
    explicit SqliteInviteRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<std::optional<InviteInfo>> find(InviteId) override;
    Result<Page<InviteInfo, InviteListPosition>> list(PageSize, std::optional<InviteListPosition>) override;
    Result<void> insert(const NewInvite&) override;
    Result<InviteInfo> consume(const InviteDigest&, UtcTime) override;
    Result<bool> revoke(InviteId, UtcTime) override;
private:
    SqliteTransactionContext& context_;

};
}
