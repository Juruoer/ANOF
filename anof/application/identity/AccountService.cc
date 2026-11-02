#include <anof/application/identity/AccountService.h>
#include <anof/application/ports/TransferMetadataStore.h>

namespace anof {
namespace {
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
Result<UserProfile> profile(UserReader& users, UserId id, ErrorCode missing = ErrorCode::NotFound) {
    auto found = users.find(id);
    if (!found) return found.error();
    if (!found.value()) return missing;
    return std::move(*found.value());
}
Result<AdminUserView> adminView(UserReader& users, SpaceReader& spaces, UserId id) {
    auto user = profile(users, id);
    if (!user) return user.error();
    auto space = spaces.findPrivate(id);
    if (!space) return space.error();
    if (!space.value() || space.value()->kind != SpaceKind::Private || space.value()->ownerId != id)
        return ErrorCode::IntegrityError;
    return AdminUserView{ std::move(user).value(), space.value()->id };
}
Result<void> match(const AccountCondition& expected, const UserProfile& user) {
    auto value = std::get_if<UserCondition>(&expected);
    if (!value || value->id != user.id || value->revision != user.revision) return ErrorCode::PreconditionFailed;
    return {};
}
Result<ManagedQuota> quota(UserReader& users, SpaceReader& spaces, QuotaReader& quotas, const QuotaScope& scope) {
    const auto spaceId = std::visit([](const auto& value) { return value.spaceId; }, scope);
    auto space = spaces.find(spaceId);
    if (!space) return space.error();
    if (!space.value()) return ErrorCode::NotFound;
    if (auto contribution = std::get_if<PublicContributionQuota>(&scope)) {
        if (space.value()->kind != SpaceKind::Public) return ErrorCode::NotFound;
        auto user = profile(users, contribution->userId);
        if (!user) return user.error();
    }
    auto found = quotas.get(scope);
    if (!found) return found.error();
    return ManagedQuota{ scope, found.value() };
}
}  // namespace
Result<UserProfile> AccountService::execute(const RequestContext& context, UpdateDisplayNameCommand command) {
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto actor = authorize(tx->authorization(), context, clock_);
    if (!actor) return actor.error();
    auto before = profile(tx->users(), actor.value().userId, ErrorCode::IntegrityError);
    if (!before) return before.error();
    if (auto matched = match(command.expected, before.value()); !matched) return matched.error();
    auto changed = tx->users().setDisplayName(before.value().id, before.value().revision, std::move(command.displayName), clock_.now());
    if (!changed) return changed.error();
    if (changed.value().revision != before.value().revision) {
        if (auto audited = tx->identityAudit().append({ context.requestId, clock_.now(), DisplayNameChanged{ before.value().id } }); !audited) return audited.error();
    }
    if (auto valid = recheck(*tx, context, clock_, actor.value()); !valid) return valid.error();
    if (auto committed = tx->commit(); !committed) return committed.error();
    return std::move(changed).value();
}
Result<Page<UserProfile, UserCursor>> AccountService::execute(const RequestContext& context, ListUsersCommand command) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    auto& read = opened.value();
    if (!read) return ErrorCode::InternalError;
    auto actor = authorize(read->authorization(), context, clock_, true);
    if (!actor) return actor.error();
    if (command.after && command.after->userId != actor.value().userId) return ErrorCode::InvalidArgument;
    auto page = read->users().list(command.size, command.after ? std::optional<UserListPosition>{ command.after->position } : std::nullopt);
    if (!page) return page.error();
    return Page<UserProfile, UserCursor>{ std::move(page.value().items), page.value().next ? std::optional<UserCursor>{ { actor.value().userId, *page.value().next } } : std::nullopt };
}
Result<AdminUserView> AccountService::execute(const RequestContext& context, GetUserCommand command) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    auto& read = opened.value();
    if (!read) return ErrorCode::InternalError;
    auto actor = authorize(read->authorization(), context, clock_, true);
    if (!actor) return actor.error();
    return adminView(read->users(), read->spaces(), command.id);
}
Result<AdminUserView> AccountService::execute(const RequestContext& context, SetUserEnabledCommand command) {
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto actor = authorize(tx->authorization(), context, clock_, true);
    if (!actor) return actor.error();
    auto before = adminView(tx->users(), tx->spaces(), command.id);
    if (!before) return before.error();
    if (auto matched = match(command.expected, before.value().user); !matched) return matched.error();
    auto changed = tx->users().setEnabled(command.id, before.value().user.revision, command.enabled, clock_.now());
    if (!changed) return changed.error();
    if (changed.value().revision != before.value().user.revision) {
        if (!command.enabled) {
            // 禁用和上传取消意图共享同一个事务；缺少能力时拒绝整个写入。
            auto* transfer = dynamic_cast<TransferTransaction*>(tx.get());
            if (!transfer) return ErrorCode::InternalError;
            auto cancelled = transfer->uploads().requestUserCancellation(command.id);
            if (!cancelled) return cancelled.error();
            auto revoked = tx->sessions().revokeAll(command.id, clock_.now());
            if (!revoked) return revoked.error();
        }
        IdentityAction event = command.enabled ? IdentityAction{ UserEnabled{ actor.value().userId, command.id } } : IdentityAction{ UserDisabled{ actor.value().userId, command.id } };
        if (auto audited = tx->identityAudit().append({ context.requestId, clock_.now(), event }); !audited) return audited.error();
    }
    if (auto valid = recheck(*tx, context, clock_, actor.value()); !valid) return valid.error();
    if (auto committed = tx->commit(); !committed) return committed.error();
    return AdminUserView{ std::move(changed).value(), before.value().privateSpaceId };
}
Result<ManagedQuota> AccountService::execute(const RequestContext& context, GetQuotaCommand command) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    auto& read = opened.value();
    if (!read) return ErrorCode::InternalError;
    auto actor = authorize(read->authorization(), context, clock_, true);
    if (!actor) return actor.error();
    return quota(read->users(), read->spaces(), read->quotas(), command.scope);
}
Result<ManagedQuota> AccountService::execute(const RequestContext& context, SetQuotaLimitCommand command) {
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto actor = authorize(tx->authorization(), context, clock_, true);
    if (!actor) return actor.error();
    auto before = quota(tx->users(), tx->spaces(), tx->quotas(), command.scope);
    if (!before) return before.error();
    auto expected = std::get_if<QuotaCondition>(&command.expected);
    if (!expected || !(expected->scope == command.scope) || expected->revision != before.value().quota.revision)
        return ErrorCode::PreconditionFailed;
    auto changed = tx->quotas().setLimit(command.scope, expected->revision, command.limit);
    if (!changed) return changed.error();
    if (changed.value().revision != before.value().quota.revision) {
        if (auto audited = tx->identityAudit().append({ context.requestId, clock_.now(), QuotaChanged{ actor.value().userId, command.scope } }); !audited) return audited.error();
    }
    if (auto valid = recheck(*tx, context, clock_, actor.value()); !valid) return valid.error();
    if (auto committed = tx->commit(); !committed) return committed.error();
    return ManagedQuota{ command.scope, changed.value() };
}
Result<PasswordChangePreparation> AccountService::preparePasswordChange(const RequestContext& context, const AccountCondition& expected) {
    auto opened = store_.beginRead();
    if (!opened) return opened.error();
    auto& read = opened.value();
    if (!read) return ErrorCode::InternalError;
    auto actor = authorize(read->authorization(), context, clock_);
    if (!actor) return actor.error();
    auto user = profile(read->users(), actor.value().userId, ErrorCode::IntegrityError);
    if (!user) return user.error();
    if (auto matched = match(expected, user.value()); !matched) return matched.error();
    auto password = read->users().findPasswordById(user.value().id);
    if (!password) return password.error();
    if (!password.value()) return ErrorCode::IntegrityError;
    return PasswordChangePreparation{ std::move(user.value().login), std::move(*password.value()) };
}
Result<void> AccountService::completePasswordChange(const RequestContext& context, const AccountCondition& expected,
                                                    const PasswordChangePreparation& prepared, std::optional<EncodedPasswordHash> hash) {
    auto opened = store_.beginWrite();
    if (!opened) return opened.error();
    auto tx = std::move(opened).value();
    if (!tx) return ErrorCode::InternalError;
    auto actor = authorize(tx->authorization(), context, clock_);
    if (!actor) return actor.error();
    if (actor.value().userId != prepared.password.userId || actor.value().authEpoch != prepared.password.authEpoch)
        return ErrorCode::Unauthenticated;
    auto user = profile(tx->users(), actor.value().userId, ErrorCode::IntegrityError);
    if (!user) return user.error();
    if (auto matched = match(expected, user.value()); !matched) return matched.error();
    auto current = tx->users().findPasswordById(actor.value().userId);
    if (!current) return current.error();
    if (!current.value()) return ErrorCode::IntegrityError;
    const auto& prior = prepared.password;
    if (!current.value()->enabled || current.value()->passwordVersion != prior.passwordVersion ||
        current.value()->authEpoch != prior.authEpoch || current.value()->userRevision != prior.userRevision)
        return ErrorCode::PreconditionFailed;
    if (!hash) return ErrorCode::Forbidden;
    // 最后一次调用方认证必须早于本操作递增 epoch 和撤销自身 Session。
    if (auto valid = recheck(*tx, context, clock_, actor.value()); !valid) return valid.error();
    auto changed = tx->users().replacePassword(actor.value().userId,
                                               { prior.passwordVersion, prior.authEpoch, prior.userRevision }, *hash, clock_.now());
    if (!changed) return changed.error();
    auto revoked = tx->sessions().revokeAll(actor.value().userId, clock_.now());
    if (!revoked) return revoked.error();
    if (auto audited = tx->identityAudit().append({ context.requestId, clock_.now(), PasswordChanged{ actor.value().userId } }); !audited) return audited.error();
    return tx->commit();
}
}  // namespace anof
