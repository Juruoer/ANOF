#pragma once
#include <anof/application/identity/IdentityCommands.h>
#include <anof/application/ports/CredentialIssuer.h>
#include <anof/application/ports/IdentityIdGenerator.h>
#include <anof/application/ports/InviteTokenCodec.h>
#include <anof/application/ports/MetadataStore.h>

namespace anof {
// 仅 metadata worker 使用。密码阶段通过自有快照跨越，绝不返回活跃仓储或事务。
class IdentityService {
public:
    IdentityService(MetadataStore& store, Clock& clock, IdentityIdGenerator& ids, CredentialIssuer& issuer, InviteTokenCodec& invites)
        : store_(store), clock_(clock), ids_(ids), issuer_(issuer), invites_(invites) {}
    Result<std::optional<PasswordSnapshot>> prepareLogin(const LoginName& login);
    Result<LoginResult> completeLogin(RequestId, const LoginCommand&, const std::optional<PasswordSnapshot>&, bool verified);
    Result<UserProfile> registerUser(RequestId, const RegisterCommand&, EncodedPasswordHash);
    Result<void> cleanupSessions();
    Result<void> execute(const RequestContext&, LogoutCommand);
    Result<CurrentUserView> execute(const RequestContext&, GetMeCommand);
    Result<CreatedInvite> execute(const RequestContext&, CreateInviteCommand);
    Result<Page<InviteInfo, InviteCursor>> execute(const RequestContext&, ListInvitesCommand);
    Result<void> execute(const RequestContext&, RevokeInviteCommand);
    Result<std::vector<SpaceView>> execute(const RequestContext&, ListSpacesCommand);
    Result<NodeDetails> execute(const RequestContext&, GetNodeCommand);
    Result<Page<NodeDetails, ChildrenCursor>> execute(const RequestContext&, ListChildrenCommand);

private:
    MetadataStore& store_;
    Clock& clock_;
    IdentityIdGenerator& ids_;
    CredentialIssuer& issuer_;
    InviteTokenCodec& invites_;
};
}  // namespace anof
