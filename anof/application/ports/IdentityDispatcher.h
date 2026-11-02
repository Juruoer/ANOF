#pragma once

#include <anof/application/identity/AccountCommands.h>
#include <anof/application/identity/IdentityCommands.h>
#include <anof/application/ports/TaskDispatcher.h>

namespace anof {
// 所属 loop 提交；自有命令移交，接受后延后完成恰一次，同步拒绝不回调。
// 回调只弱引用 HTTP；多阶段使用同一个任务身份，不持有事务等待密码计算。
class IdentityDispatcher {
public:
    virtual ~IdentityDispatcher() = default;
    virtual TaskSubmission submit(RequestId, RegisterCommand, Completion<UserProfile>) = 0;
    virtual TaskSubmission submit(RequestId, LoginCommand, Completion<LoginResult>) = 0;
    virtual TaskSubmission submit(RequestContext, LogoutCommand, Completion<void>) = 0;
    virtual TaskSubmission submit(RequestContext, GetMeCommand, Completion<CurrentUserView>) = 0;
    virtual TaskSubmission submit(RequestContext, CreateInviteCommand, Completion<CreatedInvite>) = 0;
    virtual TaskSubmission submit(RequestContext, ListInvitesCommand, Completion<Page<InviteInfo, InviteCursor>>) = 0;
    virtual TaskSubmission submit(RequestContext, RevokeInviteCommand, Completion<void>) = 0;
    virtual TaskSubmission submit(RequestContext, ListSpacesCommand, Completion<std::vector<SpaceView>>) = 0;
    virtual TaskSubmission submit(RequestContext, GetNodeCommand, Completion<NodeDetails>) = 0;
    virtual TaskSubmission submit(RequestContext, ListChildrenCommand, Completion<Page<NodeDetails, ChildrenCursor>>) = 0;
    virtual TaskSubmission submit(RequestContext, UpdateDisplayNameCommand, Completion<UserProfile>) = 0;
    virtual TaskSubmission submit(RequestContext, ChangePasswordCommand, Completion<void>) = 0;
    virtual TaskSubmission submit(RequestContext, ListUsersCommand, Completion<Page<UserProfile, UserCursor>>) = 0;
    virtual TaskSubmission submit(RequestContext, GetUserCommand, Completion<AdminUserView>) = 0;
    virtual TaskSubmission submit(RequestContext, SetUserEnabledCommand, Completion<AdminUserView>) = 0;
    virtual TaskSubmission submit(RequestContext, GetQuotaCommand, Completion<ManagedQuota>) = 0;
    virtual TaskSubmission submit(RequestContext, SetQuotaLimitCommand, Completion<ManagedQuota>) = 0;
};
}  // namespace anof
