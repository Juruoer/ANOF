#include <anof/application/identity/IdentityService.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <limits>

namespace anof {
namespace {
Result<UtcTime> later(UtcTime now, std::int64_t seconds) {
    const auto delta = seconds * 1000000; // 调用者只传至多30天，乘法不会溢出。
    if (now.time_since_epoch().count() > std::numeric_limits<std::int64_t>::max() - delta) return ErrorCode::IntegrityError;
    return UtcTime{std::chrono::microseconds{now.time_since_epoch().count() + delta}};
}
Result<Principal> authorize(AuthorizationReader& reader, const RequestContext& context, Clock& clock, bool admin = false) {
    auto user = reader.requireActiveSession(context.credential, clock.now());
    if (!user) return user.error();
    if (admin && user.value().role != UserRole::Administrator) return ErrorCode::Forbidden;
    return user;
}
Result<void> recheck(MetadataTransaction& tx, const RequestContext& context, Clock& clock, const Principal& initial) {
    auto user = authorize(tx.authorization(), context, clock);
    if (!user) return user.error();
    if (user.value().userId != initial.userId || user.value().authEpoch != initial.authEpoch || user.value().role != initial.role)
        return ErrorCode::Unauthenticated;
    return {};
}
Result<NodeDetails> visibleNode(MetadataReadSnapshot& snapshot, const Principal& principal, NodeId id) {
    auto node = snapshot.nodes().find(id);
    if (!node) return node.error();
    if (!node.value()) return ErrorCode::NotFound;
    auto space = snapshot.spaces().find(node.value()->metadata.spaceId);
    if (!space) return space.error();
    if (!space.value()) return ErrorCode::IntegrityError;
    // 读与创建具有相同的空间可见性；管理员也不能访问他人的私有空间。
    if (auto visible = requireCreatePermission(principal, *space.value()); !visible) return visible.error();
    const auto pathTarget = std::holds_alternative<DirectoryContent>(node.value()->content) ?
        std::optional<NodeId>{id} : node.value()->metadata.parentId;
    if (!pathTarget) return ErrorCode::IntegrityError;
    auto path = snapshot.nodes().findActiveDirectoryPath(space.value()->id, *pathTarget);
    if (!path) return path.error();
    // 读取允许最大深度目录；不能复用创建策略的“还需容纳下一层”检查。
    if (path.value().empty() || path.value().front().id != space.value()->rootNodeId || path.value().back().id != *pathTarget)
        return ErrorCode::IntegrityError;
    return std::move(*node.value());
}
}
Result<std::optional<PasswordSnapshot>> IdentityService::prepareLogin(const LoginName& login) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    if (!opened.value()) return ErrorCode::InternalError;
    return opened.value()->users().findPassword(login);
}
Result<LoginResult> IdentityService::completeLogin(RequestId request, const LoginCommand& command,
                                                  const std::optional<PasswordSnapshot>& snapshot, bool verified) {
    if (command.mode != CredentialMode::Bearer) return ErrorCode::InvalidArgument;
    if (!verified || !snapshot || !snapshot->enabled) return ErrorCode::Unauthenticated;
    // 随机源不在事务内执行；明文只在确定提交之后释放给调用者。
    auto token = issuer_.issueSession();
    if (!token) return token.error();
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto current = tx->users().findPasswordById(snapshot->userId);
    if (!current) return current.error();
    if (!current.value() || !current.value()->enabled || current.value()->passwordVersion != snapshot->passwordVersion ||
        current.value()->authEpoch != snapshot->authEpoch || current.value()->userRevision != snapshot->userRevision)
        return ErrorCode::Unauthenticated;
    auto user = tx->users().find(snapshot->userId);
    if (!user) return user.error();
    if (!user.value()) return ErrorCode::IntegrityError;
    const auto now = clock_.now();
    auto expiry = later(now, 7 * 86400);
    if (!expiry) return expiry.error();
    auto count = tx->sessions().countActive(snapshot->userId, now);
    if (!count) return count.error();
    if (count.value().value() >= kMaxActiveSessions) return ErrorCode::SessionLimitReached;
    if (auto inserted = tx->sessions().insert({token.value().digest, snapshot->userId, snapshot->authEpoch, command.mode, now, expiry.value()}); !inserted) return inserted.error();
    if (auto audited = tx->identityAudit().append({request, now, SessionCreated{snapshot->userId}}); !audited) return audited.error();
    LoginResult result{std::move(token.value().text), expiry.value(), std::move(*user.value())};
    if (auto committed = tx->commit(); !committed) return committed.error();
    return result;
}
Result<UserProfile> IdentityService::registerUser(RequestId request, const RegisterCommand& command, EncodedPasswordHash hash) {
    auto userId = ids_.nextUser(); auto spaceId = ids_.nextSpace(); auto rootId = ids_.nextNode();
    if (!userId) return userId.error();
    if (!spaceId) return spaceId.error();
    if (!rootId) return rootId.error();
    auto digest = invites_.digest(command.invite);
    if (!digest) return digest.error();
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto settings = tx->settings().get();
    if (!settings) return settings.error();
    if (!settings.value()) return ErrorCode::IntegrityError;
    auto count = tx->users().countUsers();
    if (!count) return count.error();
    if (count.value().value() >= settings.value()->maxUsers.value()) return ErrorCode::UserLimitReached;
    const auto now = clock_.now();
    auto invite = tx->invites().consume(digest.value(), now);
    if (!invite) return invite.error();
    NewUser user{{userId.value(), command.login, command.displayName, UserRole::Member, true, AuthEpoch{}, Revision{}, now, now},
        std::move(hash), PasswordVersion{}};
    if (auto inserted = tx->users().insert(user); !inserted) return inserted.error();
    if (auto inserted = tx->spaces().insert({spaceId.value(), SpaceKind::Private, userId.value(), rootId.value()}); !inserted) return inserted.error();
    if (auto inserted = tx->nodes().insertRoot({rootId.value(), spaceId.value(), userId.value(), now}); !inserted) return inserted.error();
    QuotaSnapshot privateQuota{{settings.value()->defaultPrivate, {ByteCount{}, NodeCount::from(1).value()}, {}}, Revision{}};
    if (auto inserted = tx->quotas().insert(SpaceQuota{spaceId.value()}, privateQuota); !inserted) return inserted.error();
    auto spaces = tx->spaces().listAccessible(userId.value());
    if (!spaces) return spaces.error();
    bool publicFound = false;
    for (const auto& space : spaces.value()) if (space.kind == SpaceKind::Public) {
        if (publicFound) return ErrorCode::IntegrityError;
        publicFound = true;
        QuotaSnapshot contribution{{settings.value()->defaultContribution, {}, {}}, Revision{}};
        if (auto inserted = tx->quotas().insert(PublicContributionQuota{space.id, userId.value()}, contribution); !inserted) return inserted.error();
    }
    if (!publicFound) return ErrorCode::IntegrityError;
    const auto checkedAt = clock_.now();
    if (checkedAt >= invite.value().expiresAt) return ErrorCode::InviteUnavailable;
    if (auto audited = tx->identityAudit().append({request, checkedAt, UserRegistered{userId.value(), invite.value().id}}); !audited) return audited.error();
    UserProfile result = std::move(user.profile);
    if (auto committed = tx->commit(); !committed) return committed.error();
    return result;
}
Result<void> IdentityService::cleanupSessions() {
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto cleaned = tx->sessions().eraseInactive(clock_.now(), PageSize::from(200).value());
    if (!cleaned) return cleaned.error();
    return tx->commit();
}
Result<void> IdentityService::execute(const RequestContext& context, LogoutCommand) {
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto user = authorize(tx->authorization(), context, clock_);
    if (!user) return user.error();
    if (auto valid = recheck(*tx, context, clock_, user.value()); !valid) return valid.error();
    auto changed = tx->sessions().revoke(context.credential, user.value().userId, clock_.now());
    if (!changed) return changed.error();
    if (changed.value()) {
        if (auto audited = tx->identityAudit().append({context.requestId, clock_.now(), SessionRevoked{user.value().userId}}); !audited) return audited.error();
    }
    return tx->commit();
}
Result<UserProfile> IdentityService::execute(const RequestContext& context, GetMeCommand) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    auto& snapshot = opened.value();
    if (!snapshot) return ErrorCode::InternalError;
    auto user = authorize(snapshot->authorization(), context, clock_);
    if (!user) return user.error();
    auto profile = snapshot->users().find(user.value().userId);
    if (!profile) return profile.error();
    if (!profile.value()) return ErrorCode::IntegrityError;
    return std::move(*profile.value());
}
Result<CreatedInvite> IdentityService::execute(const RequestContext& context, CreateInviteCommand command) {
    auto id = ids_.nextInvite();
    if (!id) return id.error();
    auto token = issuer_.issueInvite();
    if (!token) return token.error();
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto user = authorize(tx->authorization(), context, clock_, true);
    if (!user) return user.error();
    const auto now = clock_.now();
    auto expiry = later(now, command.validFor.value());
    if (!expiry) return expiry.error();
    InviteInfo info{id.value(), user.value().userId, now, expiry.value(), command.maxUses, InviteUseCount{}, std::nullopt};
    if (auto inserted = tx->invites().insert({info, token.value().digest}); !inserted) return inserted.error();
    if (auto valid = recheck(*tx, context, clock_, user.value()); !valid) return valid.error();
    if (auto audited = tx->identityAudit().append({context.requestId, clock_.now(), InviteCreated{user.value().userId, id.value()}}); !audited) return audited.error();
    CreatedInvite result{info, std::move(token.value().text)};
    if (auto committed = tx->commit(); !committed) return committed.error();
    return result;
}
Result<Page<InviteInfo, InviteCursor>> IdentityService::execute(const RequestContext& context, ListInvitesCommand command) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    auto& snapshot = opened.value();
    if (!snapshot) return ErrorCode::InternalError;
    auto user = authorize(snapshot->authorization(), context, clock_, true);
    if (!user) return user.error();
    if (command.after && command.after->userId != user.value().userId) return ErrorCode::InvalidArgument;
    auto page = snapshot->invites().list(command.size, command.after ? std::optional<InviteListPosition>{command.after->position} : std::nullopt);
    if (!page) return page.error();
    return Page<InviteInfo, InviteCursor>{std::move(page.value().items), page.value().next ?
        std::optional<InviteCursor>{{user.value().userId, *page.value().next}} : std::nullopt};
}
Result<void> IdentityService::execute(const RequestContext& context, RevokeInviteCommand command) {
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto user = authorize(tx->authorization(), context, clock_, true);
    if (!user) return user.error();
    auto changed = tx->invites().revoke(command.id, clock_.now());
    if (!changed) return changed.error();
    if (auto valid = recheck(*tx, context, clock_, user.value()); !valid) return valid.error();
    if (changed.value()) {
        if (auto audited = tx->identityAudit().append({context.requestId, clock_.now(), InviteRevoked{user.value().userId, command.id}}); !audited) return audited.error();
    }
    return tx->commit();
}
Result<std::vector<SpaceView>> IdentityService::execute(const RequestContext& context, ListSpacesCommand) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    auto& snapshot = opened.value();
    if (!snapshot) return ErrorCode::InternalError;
    auto user = authorize(snapshot->authorization(), context, clock_);
    if (!user) return user.error();
    auto spaces = snapshot->spaces().listAccessible(user.value().userId);
    if (!spaces) return spaces.error();
    std::vector<SpaceView> result;
    for (auto& space : spaces.value()) {
        auto quota = snapshot->quotas().get(SpaceQuota{space.id});
        if (!quota) return quota.error();
        std::optional<QuotaSnapshot> contribution;
        if (space.kind == SpaceKind::Public) {
            auto found = snapshot->quotas().get(PublicContributionQuota{space.id, user.value().userId});
            if (!found) return found.error();
            contribution = found.value();
        }
        result.push_back({std::move(space), quota.value(), contribution});
    }
    return result;
}
Result<NodeDetails> IdentityService::execute(const RequestContext& context, GetNodeCommand command) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    if (!opened.value()) return ErrorCode::InternalError;
    auto user = authorize(opened.value()->authorization(), context, clock_);
    if (!user) return user.error();
    return visibleNode(*opened.value(), user.value(), command.id);
}
Result<Page<NodeDetails, ChildrenCursor>> IdentityService::execute(const RequestContext& context, ListChildrenCommand command) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    auto& snapshot = opened.value();
    if (!snapshot) return ErrorCode::InternalError;
    auto user = authorize(snapshot->authorization(), context, clock_);
    if (!user) return user.error();
    auto parent = visibleNode(*snapshot, user.value(), command.parent);
    if (!parent) return parent.error();
    if (!std::holds_alternative<DirectoryContent>(parent.value().content)) return ErrorCode::ParentNotDirectory;
    const auto space = parent.value().metadata.spaceId;
    if (command.after && (command.after->userId != user.value().userId || command.after->spaceId != space || command.after->parent != command.parent))
        return ErrorCode::InvalidArgument;
    auto page = snapshot->nodes().listChildren(space, command.parent, command.size,
        command.after ? std::optional<ChildPosition>{command.after->position} : std::nullopt);
    if (!page) return page.error();
    return Page<NodeDetails, ChildrenCursor>{std::move(page.value().items), page.value().next ?
        std::optional<ChildrenCursor>{{user.value().userId, space, command.parent, *page.value().next}} : std::nullopt};
}
}  // namespace anof
