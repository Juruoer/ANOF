#pragma once

#include <anof/core/Page.h>
#include <anof/domain/identity/User.h>

#include <optional>

namespace anof {
class UserReader {
public:
    virtual ~UserReader() = default;
    virtual Result<std::optional<UserProfile>> find(UserId) = 0;
    // 包含管理员和禁用账户；按 UserId 升序、严格晚于 after，最多 size 项。
    // 仅仍有后续项时 next 为本页末项位置；返回自有资料，不含密码快照。
    // 调用方在同一快照或事务内授权；不提供跨页快照保证。
    virtual Result<Page<UserProfile, UserListPosition>> list(
        PageSize size, std::optional<UserListPosition> after) = 0;
    virtual Result<std::optional<PasswordSnapshot>> findPassword(LoginName) = 0;
    virtual Result<std::optional<PasswordSnapshot>> findPasswordById(UserId) = 0;
    virtual Result<UserCount> countUsers() = 0;
};
// 所有方法借用所属事务；唯一性、条件版本与身份复验不能跨事务拆开。
// 更新方法的UtcTime为原始观测值：适配器在同一事务按nondecreasingEventTime(now, createdAt,
// updatedAt)持久化并返回更新时间。无操作不得改时间/revision；条件失败不得写入。
// 新用户createdAt==updatedAt；此规则不改变Session/Invite到期判断。
class UserRepository : public UserReader {
public:
    virtual Result<void> insert(const NewUser&) = 0;
    virtual Result<UserProfile> setDisplayName(UserId, Revision expected, DisplayName, UtcTime) = 0;
    virtual Result<UserProfile> setEnabled(UserId, Revision expected, bool enabled, UtcTime) = 0;
    virtual Result<UserProfile> replacePassword(UserId, const PasswordExpectation&, const EncodedPasswordHash&, UtcTime) = 0;
};
}  // namespace anof
