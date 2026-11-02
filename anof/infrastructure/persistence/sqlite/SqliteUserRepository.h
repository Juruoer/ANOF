#pragma once

#include <anof/application/ports/UserRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {
// 仓储借用所属事务；不独立打开连接或提交，引用不得超过上下文寿命。
class SqliteUserRepository final : public UserRepository {
public:
    SqliteUserRepository(SqliteTransactionContext& context, PasswordHashLimits limits) : context_(context), limits_(limits) {}
    Result<std::optional<UserProfile>> find(UserId) override;
    Result<Page<UserProfile, UserListPosition>> list(PageSize, std::optional<UserListPosition>) override;
    Result<std::optional<PasswordSnapshot>> findPassword(LoginName) override;
    Result<std::optional<PasswordSnapshot>> findPasswordById(UserId) override;
    Result<UserCount> countUsers() override;
    Result<void> insert(const NewUser&) override;
    Result<UserProfile> setDisplayName(UserId, Revision, DisplayName, UtcTime) override;
    Result<UserProfile> setEnabled(UserId, Revision, bool, UtcTime) override;
    Result<UserProfile> replacePassword(UserId, const PasswordExpectation&, const EncodedPasswordHash&, UtcTime) override;

private:
    SqliteTransactionContext& context_;
    Result<std::optional<PasswordSnapshot>> password(std::string_view field, std::string_view key);
    PasswordHashLimits limits_;
};
}  // namespace anof::infrastructure::sqlite
