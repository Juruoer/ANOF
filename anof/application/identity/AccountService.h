#pragma once

#include <anof/application/RequestContext.h>
#include <anof/application/identity/AccountCommands.h>
#include <anof/application/ports/MetadataStore.h>

namespace anof {
// 自有值；不保留数据库快照。只在 metadata worker 准备和最终复验。
struct PasswordChangePreparation {
    LoginName login;
    PasswordSnapshot password;
};
class AccountService {
public:
    AccountService(MetadataStore& store, Clock& clock) : store_(store), clock_(clock) {}
    Result<UserProfile> execute(const RequestContext&, UpdateDisplayNameCommand);
    Result<Page<UserProfile, UserCursor>> execute(const RequestContext&, ListUsersCommand);
    Result<AdminUserView> execute(const RequestContext&, GetUserCommand);
    Result<AdminUserView> execute(const RequestContext&, SetUserEnabledCommand);
    Result<ManagedQuota> execute(const RequestContext&, GetQuotaCommand);
    Result<ManagedQuota> execute(const RequestContext&, SetQuotaLimitCommand);
    Result<PasswordChangePreparation> preparePasswordChange(const RequestContext&, const AccountCondition&);
    // hash 缺省表示旧密码验证失败；仍先最终认证，避免将已失效 Session 报为密码错误。
    Result<void> completePasswordChange(const RequestContext&, const AccountCondition&,
        const PasswordChangePreparation&, std::optional<EncodedPasswordHash> hash);
private:
    MetadataStore& store_;
    Clock& clock_;
};
}  // namespace anof
