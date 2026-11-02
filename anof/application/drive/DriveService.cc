#include <anof/application/drive/DriveService.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <anof/application/drive/PurgeTree.h>
#include <algorithm>

namespace anof {
namespace {
Result<Space> accessible(SpaceReader& spaces, const Principal& actor, SpaceId id) {
    auto space = spaces.find(id);
    if (!space) return space.error();
    if (!space.value()) return ErrorCode::NotFound;
    auto allowed = requireCreatePermission(actor, *space.value());
    if (!allowed) return allowed.error();
    return *space.value();
}
Result<NodeDetails> visible(NodeReader& nodes, SpaceReader& spaces, const Principal& actor, NodeId id) {
    auto node = nodes.find(id);
    if (!node) return node.error();
    if (!node.value()) return ErrorCode::NotFound;
    auto space = accessible(spaces, actor, node.value()->metadata.spaceId);
    if (!space) return space.error();
    return *node.value();
}
Result<TrashEntry> visible(TrashReader& trash, SpaceReader& spaces, const Principal& actor, TrashId id) {
    auto entry = trash.find(id);
    if (!entry) return entry.error();
    if (!entry.value()) return ErrorCode::NotFound;
    auto space = accessible(spaces, actor, entry.value()->spaceId);
    if (!space) return space.error();
    if (space.value().kind == SpaceKind::Public && actor.role != UserRole::Administrator &&
        entry.value()->soleCreatorId != actor.userId) return ErrorCode::NotFound;
    return *entry.value();
}
Result<InstanceId> instance(SettingsReader& settings) {
    auto value = settings.get();
    if (!value) return value.error();
    if (!value.value()) return ErrorCode::IntegrityError;
    return value.value()->instanceId;
}
Result<void> finish(DriveTransaction& tx, const RequestContext& ctx, const Principal& actor, Clock& clock) {
    auto final = tx.authorization().requireActiveSession(ctx.credential, clock.now());
    if (!final) return final.error();
    if (final.value().userId != actor.userId || final.value().authEpoch != actor.authEpoch || final.value().role != actor.role)
        return ErrorCode::Unauthenticated;
    return tx.commit();
}
Result<void> condition(const DriveCondition& value, const NodeDetails& n) {
    const auto* c = std::get_if<NodeCondition>(&value);
    if (!c || c->id != n.metadata.id || c->revision != n.metadata.revision) return ErrorCode::PreconditionFailed;
    return {};
}
Result<void> condition(const DriveCondition& value, const TrashEntry& t) {
    const auto* c = std::get_if<TrashCondition>(&value);
    if (!c || c->id != t.id || c->revision != t.revision) return ErrorCode::PreconditionFailed;
    return {};
}
Result<void> event(DriveTransaction& tx, const RequestContext& ctx, const Principal& actor, Clock& clock, DriveAction action) {
    return tx.driveAudit().append({ctx.requestId, clock.now(), actor.userId, std::move(action)});
}
} // namespace
Result<NodeDetails> DriveService::execute(const RequestContext& ctx, const UpdateNodeCommand& c) {
    auto opened = store_.beginDriveWrite(); if (!opened) return opened.error(); auto& tx = *opened.value();
    auto actor = tx.authorization().requireActiveSession(ctx.credential, clock_.now()); if (!actor) return actor.error();
    auto node = visible(tx.nodes(), tx.spaces(), actor.value(), c.id); if (!node) return node.error();
    Result<void> valid;
    auto space = accessible(tx.spaces(), actor.value(), node.value().metadata.spaceId); if (!space) return space.error();
    auto tree = tx.tree().activeSubtree(c.id, {}); if (!tree) return tree.error();
    valid = requireTreeMutation(actor.value(), space.value(), tree.value()); if (!valid) return valid.error();
    valid = validateDriveCommand(c); if (!valid) return valid.error();
    valid = condition(c.condition, node.value()); if (!valid) return valid.error();
    const auto before = node.value().metadata;
    if (!before.parentId) return ErrorCode::Forbidden;
    const auto parent = c.parent.value_or(*before.parentId);
    auto destination = visible(tx.nodes(), tx.spaces(), actor.value(), parent);
    if (!destination) return destination.error();
    auto moved = tx.tree().move({tree.value(), std::get<NodeCondition>(c.condition), parent, c.name.value_or(before.name), clock_.now()});
    if (!moved) return moved.error();
    if (moved.value().metadata.revision != before.revision) {
        if (parent != *before.parentId) valid = event(tx, ctx, actor.value(), clock_, NodeMoved{c.id, *before.parentId, parent});
        else valid = event(tx, ctx, actor.value(), clock_, NodeRenamed{c.id});
        if (!valid) return valid.error();
    }
    valid = finish(tx, ctx, actor.value(), clock_); if (!valid) return valid.error();
    return std::move(moved).value();
}
Result<NodeDetails> DriveService::execute(const RequestContext& ctx, const CopyFileCommand& c) {
    auto opened = store_.beginDriveWrite(); if (!opened) return opened.error(); auto& tx = *opened.value();
    auto actor = tx.authorization().requireActiveSession(ctx.credential, clock_.now()); if (!actor) return actor.error();
    auto source = visible(tx.nodes(), tx.spaces(), actor.value(), c.id); if (!source) return source.error();
    auto valid = validateDriveCommand(c); if (!valid) return valid.error();
    valid = condition(c.condition, source.value()); if (!valid) return valid.error();
    if (!std::holds_alternative<FileContentMetadata>(source.value().content)) return ErrorCode::NotAFile;
    auto space = accessible(tx.spaces(), actor.value(), c.space); if (!space) return space.error();
    auto path = tx.nodes().findActiveDirectoryPath(c.space, c.parent); if (!path) return path.error();
    valid = validateDirectoryParent(space.value(), c.parent, path.value()); if (!valid) return valid.error();
    auto content = tx.blobs().findByNode(c.id); if (!content) return content.error();
    if (!content.value()) return ErrorCode::IntegrityError;
    auto blob = tx.blobs().record(content.value()->id); if (!blob) return blob.error();
    if (!blob.value()) return ErrorCode::IntegrityError;
    if (blob.value()->state == BlobState::Unavailable) return ErrorCode::IntegrityError;
    if (blob.value()->state != BlobState::Available) return ErrorCode::Retryable;
    auto id = nodes_.next(); if (!id) return id.error();
    auto scopes = directoryQuotaScopes(actor.value(), space.value()); if (!scopes) return scopes.error();
    const QuotaDelta delta{ByteCount::from(content.value()->size.value()).value(), NodeCount::from(1).value()};
    for (const auto& scope : scopes.value()) { valid = tx.quotas().chargeUsed(scope, delta); if (!valid) return valid.error(); }
    valid = tx.nodeAccounting().chargeUsed(delta.nodes); if (!valid) return valid.error();
    const auto at = std::max(clock_.now(), blob.value()->createdAt);
    auto result = tx.nodes().insertFile({{id.value(), c.space, c.parent, c.name, actor.value().userId, at}, content.value()->id});
    if (!result) return result.error();
    auto rev = tx.nodes().advanceRevision(c.parent, path.value().back().revision, at); if (!rev) return rev.error();
    valid = event(tx, ctx, actor.value(), clock_, FileCopied{c.id, id.value()}); if (!valid) return valid.error();
    valid = finish(tx, ctx, actor.value(), clock_); if (!valid) return valid.error();
    return std::move(result).value();
}
Result<TrashView> DriveService::execute(const RequestContext& ctx, const TrashNodeCommand& c) {
    auto opened = store_.beginDriveWrite(); if (!opened) return opened.error(); auto& tx = *opened.value();
    auto actor = tx.authorization().requireActiveSession(ctx.credential, clock_.now()); if (!actor) return actor.error();
    auto node = visible(tx.nodes(), tx.spaces(), actor.value(), c.id); if (!node) return node.error();
    Result<void> valid;
    auto space = accessible(tx.spaces(), actor.value(), node.value().metadata.spaceId); if (!space) return space.error();
    auto tree = tx.tree().activeSubtree(c.id, {}); if (!tree) return tree.error();
    valid = requireTreeMutation(actor.value(), space.value(), tree.value()); if (!valid) return valid.error();
    valid = condition(c.condition, node.value()); if (!valid) return valid.error();
    auto summary = summarizeTree(tree.value()); if (!summary) return summary.error();
    const auto at = std::max(clock_.now(), summary.value().latestEvent);
    auto expiry = trashExpiry(at); if (!expiry) return expiry.error();
    auto id = ids_.trashId(); if (!id) return id.error();
    auto identity = instance(tx.settings()); if (!identity) return identity.error();
    const auto& n = node.value().metadata;
    if (!n.parentId) return ErrorCode::Forbidden;
    TrashEntry entry{id.value(), n.spaceId, c.id, *n.parentId, n.name,
        std::holds_alternative<DirectoryContent>(node.value().content) ? NodeKind::Directory : NodeKind::File,
        actor.value().userId, at, expiry.value(), {}, summary.value().total.nodes, summary.value().total.bytes, summary.value().soleCreator};
    valid = tx.trash().insert(entry); if (!valid) return valid.error();
    valid = tx.tree().detach(tree.value(), std::get<NodeCondition>(c.condition), entry); if (!valid) return valid.error();
    valid = event(tx, ctx, actor.value(), clock_, TreeTrashed{entry.id, c.id}); if (!valid) return valid.error();
    TrashView result{identity.value(), std::move(entry)};
    valid = finish(tx, ctx, actor.value(), clock_); if (!valid) return valid.error();
    return result;
}
Result<NodeDetails> DriveService::execute(const RequestContext& ctx, const RestoreTrashCommand& c) {
    auto opened = store_.beginDriveWrite(); if (!opened) return opened.error(); auto& tx = *opened.value();
    auto actor = tx.authorization().requireActiveSession(ctx.credential, clock_.now()); if (!actor) return actor.error();
    auto entry = visible(tx.trash(), tx.spaces(), actor.value(), c.id); if (!entry) return entry.error();
    auto valid = validateDriveCommand(c); if (!valid) return valid.error();
    valid = condition(c.condition, entry.value()); if (!valid) return valid.error();
    const auto parent = c.parent.value_or(entry.value().originalParentId);
    auto target = tx.nodes().find(parent); if (!target) return target.error();
    if (!target.value() || target.value()->metadata.spaceId != entry.value().spaceId ||
        !std::holds_alternative<DirectoryContent>(target.value()->content)) return ErrorCode::RestoreTargetUnavailable;
    auto tree = tx.tree().trashSubtree(c.id, {}); if (!tree) return tree.error();
    auto root = std::find_if(tree.value().members.begin(), tree.value().members.end(), [&](const auto& m){return m.node.metadata.id == entry.value().rootNodeId;});
    if (root == tree.value().members.end()) return ErrorCode::IntegrityError;
    auto restored = tx.tree().restore({tree.value(), {entry.value().rootNodeId, root->node.metadata.revision}, parent,
        c.name.value_or(entry.value().originalName), clock_.now()}, std::get<TrashCondition>(c.condition));
    if (!restored) return restored.error();
    valid = tx.trash().erase(std::get<TrashCondition>(c.condition)); if (!valid) return valid.error();
    valid = event(tx, ctx, actor.value(), clock_, TreeRestored{c.id, entry.value().rootNodeId}); if (!valid) return valid.error();
    valid = finish(tx, ctx, actor.value(), clock_); if (!valid) return valid.error();
    return std::move(restored).value();
}
Result<void> DriveService::execute(const RequestContext& ctx, const PurgeTrashCommand& c) {
    auto opened = store_.beginDriveWrite(); if (!opened) return opened.error(); auto& tx = *opened.value();
    auto actor = tx.authorization().requireActiveSession(ctx.credential, clock_.now()); if (!actor) return actor.error();
    auto entry = visible(tx.trash(), tx.spaces(), actor.value(), c.id); if (!entry) return entry.error();
    auto valid = condition(c.condition, entry.value()); if (!valid) return valid.error();
    valid = purgeTree(tx, entry.value(), clock_.now(), ctx.requestId, actor.value().userId);
    if (!valid) return valid.error();
    return finish(tx, ctx, actor.value(), clock_);
}
Result<TrashView> DriveService::execute(const RequestContext& ctx, const GetTrashCommand& c) {
    auto opened = store_.beginDriveRead(); if (!opened) return opened.error(); auto& tx = *opened.value();
    auto actor = tx.authorization().requireActiveSession(ctx.credential, clock_.now()); if (!actor) return actor.error();
    auto entry = visible(tx.trash(), tx.spaces(), actor.value(), c.id); if (!entry) return entry.error();
    auto identity = instance(tx.settings()); if (!identity) return identity.error();
    return TrashView{identity.value(), std::move(entry).value()};
}
Result<Page<NodeDetails, SearchCursor>> DriveService::execute(const RequestContext& ctx, const SearchNodesCommand& c) {
    auto opened = store_.beginDriveRead(); if (!opened) return opened.error(); auto& tx = *opened.value();
    auto actor = tx.authorization().requireActiveSession(ctx.credential, clock_.now()); if (!actor) return actor.error();
    auto valid = validateDriveCommand(actor.value().userId, c); if (!valid) return valid.error();
    auto page = tx.tree().search(actor.value(), {c.space, c.text, c.limit, c.after ? std::optional<ChildPosition>(c.after->position) : std::nullopt, {}});
    if (!page) return page.error();
    Page<NodeDetails, SearchCursor> result{std::move(page.value().items), {}};
    if (page.value().next) result.next = SearchCursor{actor.value().userId, c.space, c.text, *page.value().next};
    return result;
}
Result<Page<TrashView, TrashCursor>> DriveService::execute(const RequestContext& ctx, const ListTrashCommand& c) {
    auto opened = store_.beginDriveRead(); if (!opened) return opened.error(); auto& tx = *opened.value();
    auto actor = tx.authorization().requireActiveSession(ctx.credential, clock_.now()); if (!actor) return actor.error();
    auto valid = validateDriveCommand(actor.value().userId, c); if (!valid) return valid.error();
    auto identity = instance(tx.settings()); if (!identity) return identity.error();
    auto page = tx.trash().list({actor.value(), c.space, c.root, c.limit, c.after ? std::optional<TrashPosition>(c.after->position) : std::nullopt});
    if (!page) return page.error();
    Page<TrashView, TrashCursor> result;
    for (auto& entry : page.value().items) result.items.push_back({identity.value(), std::move(entry)});
    if (page.value().next) result.next = TrashCursor{actor.value().userId, c.space, c.root, *page.value().next};
    return result;
}
} // namespace anof
