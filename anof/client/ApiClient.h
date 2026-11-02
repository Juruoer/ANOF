#pragma once
#include <anof/client/ProfileStore.h>
#include <anof/client/Protocol.h>
namespace anof::client {
struct SavedLogin {
    User user;
    std::string expiresAt;
    bool credentialSaved = true;
};
struct Logout {
    std::optional<ResponseMeta> meta;
};
// 调用者持有 LockedProfile 的整个生命周期覆盖输入、请求及本地保存。
// 同步借用 transport/profile/cancel，不创建线程、不自动重试。
class ApiClient {
public:
    ApiClient(HttpTransport& transport, LockedProfile& profile, const std::atomic_bool* cancel = nullptr)
        : transport_(transport), profile_(profile), cancel_(cancel) {}
    ClientResult<Reply<User>> registerUser(const LoginName&, const DisplayName&, const PasswordInput&, const InviteTokenText&);
    ClientResult<Reply<SavedLogin>> login(const LoginName&, const PasswordInput&);
    ClientResult<Logout> logout();
    ClientResult<Reply<CurrentUser>> me();
    ClientResult<Reply<CreatedInvite>> createInvite(unsigned maxUses = 1, unsigned validForSeconds = 604800);
    ClientResult<Reply<Page<Invite>>> listInvites(PageRequest page = {});
    ClientResult<Reply<Done>> revokeInvite(InviteId);
    ClientResult<Reply<std::vector<Space>>> spaces();
    ClientResult<Reply<Page<Node>>> children(NodeId, PageRequest page = {});
    ClientResult<Reply<Node>> stat(NodeId);
    ClientResult<Reply<Node>> mkdir(SpaceId, NodeId parent, std::string_view name);
    ClientResult<Reply<User>> rename(const DisplayName&, const ConditionTag&);
    ClientResult<Reply<Done>> changePassword(const PasswordInput& current, const PasswordInput& next, const ConditionTag&);
    ClientResult<Reply<Page<User>>> listUsers(PageRequest page = {});
    ClientResult<Reply<AdminUser>> getUser(UserId);
    ClientResult<Reply<AdminUser>> setUserEnabled(UserId, bool, const ConditionTag&);
    ClientResult<Reply<ManagedQuota>> getQuota(QuotaTarget);
    ClientResult<Reply<ManagedQuota>> setQuotaLimit(QuotaTarget, Amount, const ConditionTag&);

private:
    template <class T>
    ClientResult<Reply<T>> call(Operation, Method, std::string, SecureBuffer body = {}, unsigned limit = 200, std::optional<ConditionTag> condition = {});
    HttpTransport& transport_;
    LockedProfile& profile_;
    const std::atomic_bool* cancel_;
};
}  // namespace anof::client
