#include <anof/client/ApiClient.h>
#include <anof/client/internal/Json.h>
#include <anof/domain/drive/DirectoryPolicy.h>
namespace anof::client {
namespace {
using namespace internal;
SecureBuffer object(std::initializer_list<std::pair<std::string_view, std::string_view>> fields) {
    SecureBuffer body(kRequestLimit);
    append(body, "{");
    bool first = true;
    for (const auto& field : fields) {
        if (!first) append(body, ",");
        first = false;
        quoted(body, field.first);
        append(body, ":");
        quoted(body, field.second);
    }
    append(body, "}");
    return body;
}
ClientError localAfterSuccess(ClientError error, const ResponseMeta& meta) {
    error.remote = RemoteOutcome::ConfirmedSuccess;
    error.httpStatus = meta.httpStatus;
    error.requestId = meta.requestId;
    return error;
}
}  // namespace
template <class T>
ClientResult<Reply<T>> ApiClient::call(Operation op, Method method, std::string path, SecureBuffer body, unsigned limit, std::optional<ConditionTag> condition) {
    if (!profile_.usable()) return failure(ErrorKind::LocalState, "PROFILE_RELOAD_REQUIRED");
    const bool anonymous = op == Operation::Login || op == Operation::Register;
    const auto& p = profile_.profile();
    if (!anonymous && !p.credential) return failure(ErrorKind::AuthenticationRequired);
    bool started = false;
    try {
        HttpRequest request{ method, "/api/v1" + path, std::move(body), anonymous ? nullptr : &p.credential->accessToken, std::move(condition) };
        started = true;
        auto response = transport_.perform(p.server, request, cancel_);
        if (!response) return response.error();
        auto decoded = decodeResponse(op, response.value(), limit);
        if (!decoded) return decoded.error();
        return Reply<T>{ std::get<T>(std::move(decoded.value().data)), std::move(decoded.value().meta) };
    } catch (...) {
        auto error = failure(started && method != Method::Get ? ErrorKind::OutcomeUnknown : ErrorKind::LocalState);
        if (started) error.remote = RemoteOutcome::Unknown;
        return error;
    }
}
ClientResult<Reply<User>> ApiClient::registerUser(const LoginName& login, const DisplayName& name, const PasswordInput& password, const InviteTokenText& invite) {
    auto reply = call<User>(Operation::Register, Method::Post, "/auth/register", object({ { "login", login.value() }, { "display_name", name.value() }, { "password", password.view() }, { "invite_token", invite.view() } }));
    if (reply && (!(reply.value().data.login == login) || reply.value().data.displayName.value() != name.value())) return failure(ErrorKind::OutcomeUnknown);
    return reply;
}
ClientResult<Reply<SavedLogin>> ApiClient::login(const LoginName& login, const PasswordInput& password) {
    if (!profile_.usable()) return failure(ErrorKind::LocalState, "PROFILE_RELOAD_REQUIRED");
    if (profile_.profile().credential) return failure(ErrorKind::LocalState, "CREDENTIAL_ALREADY_PRESENT");
    auto response = call<Login>(Operation::Login, Method::Post, "/auth/sessions", object({ { "login", login.value() }, { "password", password.view() }, { "credential_mode", "bearer" } }));
    if (!response) return response.error();
    auto& value = response.value();
    if (!(value.data.user.login == login)) return failure(ErrorKind::OutcomeUnknown);
    try {
        auto saved = profile_.saveCredential(StoredCredential{ std::move(value.data.accessToken), value.data.expiresAt, value.data.user.id, value.data.user.login });
        if (!saved) return localAfterSuccess(saved.error(), value.meta);
        return Reply<SavedLogin>{ { std::move(value.data.user), std::move(value.data.expiresAt), true }, std::move(value.meta) };
    } catch (...) {
        auto error = localAfterSuccess(failure(ErrorKind::LocalState), value.meta);
        error.local = LocalState::Unknown;
        return error;
    }
}
ClientResult<Logout> ApiClient::logout() {
    if (!profile_.usable()) return failure(ErrorKind::LocalState, "PROFILE_RELOAD_REQUIRED");
    if (!profile_.profile().credential) return Logout{};
    auto response = call<Done>(Operation::Logout, Method::Delete, "/auth/session");
    auto cleared = profile_.clearCredential();
    if (!cleared) {
        auto error = cleared.error();
        error.remote = response ? RemoteOutcome::ConfirmedSuccess : response.error().remote;
        error.httpStatus = response ? std::optional<int>(response.value().meta.httpStatus) : response.error().httpStatus;
        error.requestId = response ? std::optional<RequestId>(response.value().meta.requestId) : response.error().requestId;
        return error;
    }
    if (!response) {
        auto error = response.error();
        error.local = LocalState::CredentialCleared;
        return error;
    }
    return Logout{ std::move(response.value().meta) };
}
ClientResult<Reply<CurrentUser>> ApiClient::me() { return call<CurrentUser>(Operation::Me, Method::Get, "/users/me"); }
ClientResult<Reply<CreatedInvite>> ApiClient::createInvite(unsigned maxUses, unsigned validForSeconds) {
    if (maxUses < 1 || maxUses > 10000 || validForSeconds < 1 || validForSeconds > 2592000) return failure(ErrorKind::InvalidArgument);
    SecureBuffer body(kRequestLimit);
    append(body, "{\"max_uses\":" + std::to_string(maxUses) + ",\"valid_for_seconds\":" + std::to_string(validForSeconds) + "}");
    return call<CreatedInvite>(Operation::CreateInvite, Method::Post, "/admin/invites", std::move(body));
}
ClientResult<Reply<Page<Invite>>> ApiClient::listInvites(PageRequest page) {
    auto query = pageQuery(page);
    if (!query) return query.error();
    return call<Page<Invite>>(Operation::ListInvites, Method::Get, "/admin/invites" + query.value(), {}, page.limit);
}
ClientResult<Reply<Done>> ApiClient::revokeInvite(InviteId id) { return call<Done>(Operation::RevokeInvite, Method::Delete, "/admin/invites/" + id.toString()); }
ClientResult<Reply<std::vector<Space>>> ApiClient::spaces() { return call<std::vector<Space>>(Operation::Spaces, Method::Get, "/spaces"); }
ClientResult<Reply<Page<Node>>> ApiClient::children(NodeId id, PageRequest page) {
    auto query = pageQuery(page);
    if (!query) return query.error();
    auto reply = call<Page<Node>>(Operation::Children, Method::Get, "/nodes/" + id.toString() + "/children" + query.value(), {}, page.limit);
    if (reply)
        for (const auto& item : reply.value().data.items)
            if (item.parentId != id) return failure(ErrorKind::Protocol);
    return reply;
}
ClientResult<Reply<Node>> ApiClient::stat(NodeId id) {
    auto reply = call<Node>(Operation::Stat, Method::Get, "/nodes/" + id.toString());
    if (reply && reply.value().data.id != id) return failure(ErrorKind::Protocol);
    return reply;
}
ClientResult<Reply<Node>> ApiClient::mkdir(SpaceId space, NodeId parent, std::string_view name) {
    if (!validateDirectoryName(name)) return failure(ErrorKind::InvalidArgument);
    auto reply = call<Node>(Operation::Mkdir, Method::Post, "/nodes", object({ { "space_id", space.toString() }, { "parent_id", parent.toString() }, { "name", name } }));
    if (reply && (reply.value().data.spaceId != space || reply.value().data.parentId != parent || reply.value().data.name != name)) return failure(ErrorKind::OutcomeUnknown);
    return reply;
}
namespace {
std::string quotaPath(const QuotaTarget& target) {
    return "/admin/spaces/" + target.spaceId.toString() + (target.userId ? "/contributions/" + target.userId->toString() : "") + "/quota";
}
bool sameTarget(const QuotaTarget& a, const QuotaTarget& b) { return a.spaceId == b.spaceId && a.userId == b.userId; }
}  // namespace
ClientResult<Reply<User>> ApiClient::rename(const DisplayName& name, const ConditionTag& condition) {
    auto reply = call<User>(Operation::Rename, Method::Patch, "/users/me", object({ { "display_name", name.value() } }), 200, condition);
    if (reply && (reply.value().data.id != profile_.profile().credential->userId || reply.value().data.displayName.value() != name.value())) return failure(ErrorKind::OutcomeUnknown);
    return reply;
}
ClientResult<Reply<Done>> ApiClient::changePassword(const PasswordInput& current, const PasswordInput& next, const ConditionTag& condition) {
    auto reply = call<Done>(Operation::ChangePassword, Method::Post, "/users/me/password", object({ { "current_password", current.view() }, { "new_password", next.view() } }), 200, condition);
    // 只有确认成功才清除本 profile；拒绝及未知结果保留凭据，不自动重试。
    if (!reply) return reply.error();
    try {
        auto cleared = profile_.clearCredential();
        if (!cleared) return localAfterSuccess(cleared.error(), reply.value().meta);
        return reply;
    } catch (...) {
        auto error = localAfterSuccess(failure(ErrorKind::LocalState), reply.value().meta);
        error.local = LocalState::Unknown;
        return error;
    }
}
ClientResult<Reply<Page<User>>> ApiClient::listUsers(PageRequest page) {
    auto query = pageQuery(page);
    if (!query) return query.error();
    return call<Page<User>>(Operation::ListUsers, Method::Get, "/admin/users" + query.value(), {}, page.limit);
}
ClientResult<Reply<AdminUser>> ApiClient::getUser(UserId id) {
    auto reply = call<AdminUser>(Operation::GetUser, Method::Get, "/admin/users/" + id.toString());
    if (reply && reply.value().data.user.id != id) return failure(ErrorKind::Protocol);
    return reply;
}
ClientResult<Reply<AdminUser>> ApiClient::setUserEnabled(UserId id, bool enabled, const ConditionTag& condition) {
    SecureBuffer body(kRequestLimit);
    append(body, enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    auto reply = call<AdminUser>(Operation::SetUserEnabled, Method::Patch, "/admin/users/" + id.toString(), std::move(body), 200, condition);
    if (reply && (reply.value().data.user.id != id || reply.value().data.user.enabled != enabled)) return failure(ErrorKind::OutcomeUnknown);
    return reply;
}
ClientResult<Reply<ManagedQuota>> ApiClient::getQuota(QuotaTarget target) {
    auto reply = call<ManagedQuota>(Operation::GetQuota, Method::Get, quotaPath(target));
    if (reply && !sameTarget(reply.value().data.target, target)) return failure(ErrorKind::Protocol);
    return reply;
}
ClientResult<Reply<ManagedQuota>> ApiClient::setQuotaLimit(QuotaTarget target, Amount limit, const ConditionTag& condition) {
    SecureBuffer body(kRequestLimit);
    append(body, "{\"limit\":{\"bytes\":\"" + std::to_string(limit.bytes.value()) + "\",\"nodes\":\"" + std::to_string(limit.nodes.value()) + "\"}}");
    auto reply = call<ManagedQuota>(Operation::SetQuotaLimit, Method::Put, quotaPath(target), std::move(body), 200, condition);
    if (reply && (!sameTarget(reply.value().data.target, target) || reply.value().data.quota.limit.bytes != limit.bytes || reply.value().data.quota.limit.nodes != limit.nodes)) return failure(ErrorKind::OutcomeUnknown);
    return reply;
}
}  // namespace anof::client
