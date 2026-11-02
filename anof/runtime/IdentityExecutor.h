#pragma once
#include <anof/application/ports/IdentityDispatcher.h>
#include <anof/application/ports/PasswordHasher.h>
#include <anof/runtime/MetadataExecutor.h>

namespace anof::runtime {
struct PasswordWorkerResources {
    std::unique_ptr<PasswordHasher> hasher;
    EncodedPasswordHash dummy;
};
struct IdentityExecutorOptions {
    std::size_t maxOutstanding = 128;
    // 包含读快照、排队、计算及最终事务，直到完成回调退出。
    std::size_t maxPasswordRequests = 8;
};
class IdentityExecutor final : public IdentityDispatcher {
public:
    using Factory = std::function<Result<PasswordWorkerResources>()>;
    IdentityExecutor(muduox::net::EventLoop&, MetadataExecutor&, IdentityExecutorOptions = {});
    // 保持 metadata 和 loop 存活，先 requestStop 并等待 stopped，禁止在回调内析构。
    ~IdentityExecutor() override;
    Result<void> start(Factory, MetadataExecutor::Started = {}, MetadataExecutor::Stopped = {});
    void requestStop();
    void setUserDisabledObserver(std::function<void(UserId, bool)>);
    bool stopped() const;
    TaskSubmission submit(RequestId, RegisterCommand, Completion<UserProfile>) override;
    TaskSubmission submit(RequestId, LoginCommand, Completion<LoginResult>) override;
    TaskSubmission submit(RequestContext, LogoutCommand, Completion<void>) override;
    TaskSubmission submit(RequestContext, GetMeCommand, Completion<CurrentUserView>) override;
    TaskSubmission submit(RequestContext, CreateInviteCommand, Completion<CreatedInvite>) override;
    TaskSubmission submit(RequestContext, ListInvitesCommand, Completion<Page<InviteInfo, InviteCursor>>) override;
    TaskSubmission submit(RequestContext, RevokeInviteCommand, Completion<void>) override;
    TaskSubmission submit(RequestContext, ListSpacesCommand, Completion<std::vector<SpaceView>>) override;
    TaskSubmission submit(RequestContext, GetNodeCommand, Completion<NodeDetails>) override;
    TaskSubmission submit(RequestContext, ListChildrenCommand, Completion<Page<NodeDetails, ChildrenCursor>>) override;
    TaskSubmission submit(RequestContext, UpdateDisplayNameCommand, Completion<UserProfile>) override;
    TaskSubmission submit(RequestContext, ChangePasswordCommand, Completion<void>) override;
    TaskSubmission submit(RequestContext, ListUsersCommand, Completion<Page<UserProfile, UserCursor>>) override;
    TaskSubmission submit(RequestContext, GetUserCommand, Completion<AdminUserView>) override;
    TaskSubmission submit(RequestContext, SetUserEnabledCommand, Completion<AdminUserView>) override;
    TaskSubmission submit(RequestContext, GetQuotaCommand, Completion<ManagedQuota>) override;
    TaskSubmission submit(RequestContext, SetQuotaLimitCommand, Completion<ManagedQuota>) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace anof::runtime
