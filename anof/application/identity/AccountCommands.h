#pragma once

#include <anof/core/Page.h>
#include <anof/domain/drive/Quota.h>
#include <anof/domain/identity/AuthenticationSource.h>
#include <anof/domain/identity/User.h>

#include <optional>
#include <variant>

namespace anof {
// 保留条件的资源种类与绑定；应用在授权、目标检查之后比较，不能只传 revision。
struct UserCondition {
    UserId id;
    Revision revision;
};
struct QuotaCondition {
    QuotaScope scope;
    Revision revision;
};
using AccountCondition = std::variant<UserCondition, QuotaCondition>;
struct UserCursor {
    UserId userId;
    UserListPosition position;
};
struct AdminUserView {
    UserProfile user;
    SpaceId privateSpaceId;
};
struct ManagedQuota {
    QuotaScope scope;
    QuotaSnapshot quota;
};
struct UpdateDisplayNameCommand {
    AccountCondition expected;
    DisplayName displayName;
};
struct ChangePasswordCommand {
    AccountCondition expected;
    PasswordInput currentPassword;
    PasswordInput newPassword;
    AuthenticationSource source;
};
struct ListUsersCommand {
    PageSize size;
    std::optional<UserCursor> after;
};
struct GetUserCommand {
    UserId id;
};
struct SetUserEnabledCommand {
    UserId id;
    AccountCondition expected;
    bool enabled;
};
struct GetQuotaCommand {
    QuotaScope scope;
};
struct SetQuotaLimitCommand {
    QuotaScope scope;
    AccountCondition expected;
    QuotaDelta limit;
};
}  // namespace anof
