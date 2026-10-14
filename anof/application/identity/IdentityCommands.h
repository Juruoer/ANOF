#pragma once

#include <anof/application/RequestContext.h>
#include <anof/domain/identity/AuthenticationSource.h>
#include <anof/domain/identity/User.h>
#include <anof/domain/identity/Session.h>
#include <anof/domain/identity/Invite.h>
#include <anof/domain/drive/NodeDetails.h>
#include <anof/domain/drive/Space.h>
#include <anof/domain/drive/Quota.h>

namespace anof {
struct RegisterCommand {
    LoginName login;
    DisplayName displayName;
    PasswordInput password;
    InviteTokenText invite;
    AuthenticationSource source;
};
struct LoginCommand {
    LoginName login;
    PasswordInput password;
    CredentialMode mode;
    AuthenticationSource source;
};
struct LoginResult {
    SecretToken accessToken;
    UtcTime expiresAt;
    UserProfile user;
};
struct LogoutCommand {};
struct GetMeCommand {};
using InviteLifetimeSeconds = Quantity<struct InviteLifetimeTag, 1, 2592000>;
struct CreateInviteCommand {
    InviteUseLimit maxUses;
    InviteLifetimeSeconds validFor;
};
struct InviteCursor {
    UserId userId;
    InviteListPosition position;
};
struct ListInvitesCommand {
    PageSize size;
    std::optional<InviteCursor> after;
};
struct RevokeInviteCommand {
    InviteId id;
};
struct CreatedInvite {
    InviteInfo invite;
    SecretToken token;
};
struct ListSpacesCommand {};
struct SpaceView {
    Space space;
    QuotaSnapshot quota;
    std::optional<QuotaSnapshot> myContribution;
};
struct GetNodeCommand {
    NodeId id;
};
struct ChildrenCursor {
    UserId userId;
    SpaceId spaceId;
    NodeId parent;
    ChildPosition position;
};
struct ListChildrenCommand {
    NodeId parent;
    PageSize size;
    std::optional<ChildrenCursor> after;
};
// transport 检查游标编码与结构；应用在同一读快照中检查用户/空间/目录绑定及可见性。
}  // namespace anof
