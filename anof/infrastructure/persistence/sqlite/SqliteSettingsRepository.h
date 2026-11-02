#pragma once

#include <anof/application/ports/InstanceSettingsRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {
// 仓储借用所属事务；不独立打开连接或提交，引用不得超过上下文寿命。
class SqliteSettingsRepository final : public SettingsRepository {
public:
    explicit SqliteSettingsRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<std::optional<InstanceSettings>> get() override;
    Result<void> insertBootstrap(const InstanceSettings&) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
