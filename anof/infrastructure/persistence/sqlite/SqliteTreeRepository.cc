#include <anof/domain/drive/DirectoryPolicy.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteNodeRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteQueryBudget.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSpaceRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTrashRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTreeRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

#include <map>
namespace anof::infrastructure::sqlite {
namespace {
constexpr const char* columns = "n.id,n.space_id,n.parent_id,n.name,n.created_by,n.revision,n.created_at,n.updated_at,n.kind,n.blob_id,b.size,b.sha256,b.state";
Result<TreeMember> decode(SqliteStatement& s) {
    auto id = readId<NodeId>(s, 0);
    auto space = readId<SpaceId>(s, 1);
    auto nil = s.isNull(2);
    auto name = s.text(3);
    auto user = readId<UserId>(s, 4);
    auto rev = readQuantity<Revision>(s, 5);
    auto created = readTime(s, 6);
    auto updated = readTime(s, 7);
    auto kind = s.integer(8);
    if (!id || !space || !nil || !name || !user || !rev || !created || !updated || !kind || kind.value() < 0 || kind.value() > 1) return ErrorCode::IntegrityError;
    std::optional<NodeId> parent;
    if (!nil.value()) {
        auto p = readId<NodeId>(s, 2);
        if (!p) return p.error();
        parent = p.value();
    }
    TreeMember m{ { { id.value(), space.value(), parent, name.value(), user.value(), rev.value(), created.value(), updated.value() }, DirectoryContent{} }, {} };
    if (kind.value() == 1) {
        auto blob = readId<BlobId>(s, 9);
        auto size = readQuantity<ByteCount>(s, 10);
        auto hash = s.blob(11);
        auto state = s.integer(12);
        if (!blob || !size || !hash || hash.value().size() != 32 || !state || (state.value() != 0 && state.value() != 2)) return ErrorCode::IntegrityError;
        FileContentMetadata f{ size.value(), {} };
        std::copy(hash.value().begin(), hash.value().end(), f.sha256.begin());
        m.node.content = f;
        m.blobId = blob.value();
    }
    return m;
}
bool equal(const TreeMember& a, const TreeMember& b) {
    const auto& x = a.node.metadata;
    const auto& y = b.node.metadata;
    if (x.id != y.id || x.spaceId != y.spaceId || x.parentId != y.parentId || x.name != y.name || x.createdBy != y.createdBy || x.revision != y.revision || x.createdAt != y.createdAt || x.updatedAt != y.updatedAt || a.blobId != b.blobId || a.node.content.index() != b.node.content.index()) return false;
    auto f = std::get_if<FileContentMetadata>(&a.node.content);
    auto g = std::get_if<FileContentMetadata>(&b.node.content);
    return !f || (f->size == g->size && f->sha256 == g->sha256);
}
Result<void> matches(const TreeSnapshot& expected, const TreeSnapshot& actual, NodeCondition condition) {
    if (expected.root != actual.root || condition.id != actual.root) return ErrorCode::InvalidArgument;
    const auto root = std::find_if(actual.members.begin(), actual.members.end(), [&](const auto& m) { return m.node.metadata.id == actual.root; });
    if (root == actual.members.end()) return ErrorCode::IntegrityError;
    if (root->node.metadata.revision != condition.revision) return ErrorCode::PreconditionFailed;
    if (!root->node.metadata.parentId && root->node.metadata.name.empty()) return ErrorCode::Forbidden;
    auto summary = summarizeTree(actual);
    if (!summary) return summary.error();
    if (expected.members.size() != actual.members.size()) return ErrorCode::PreconditionFailed;
    std::map<NodeId, const TreeMember*> map;
    for (const auto& m : expected.members)
        if (!map.emplace(m.node.metadata.id, &m).second) return ErrorCode::InvalidArgument;
    for (const auto& m : actual.members) {
        auto it = map.find(m.node.metadata.id);
        if (it == map.end() || !equal(*it->second, m)) return ErrorCode::PreconditionFailed;
    }
    return {};
}
const DirectorySnapshot& rootOf(const TreeSnapshot& t) {
    return std::find_if(t.members.begin(), t.members.end(), [&](const auto& m) { return m.node.metadata.id == t.root; })->node.metadata;
}
Result<void> destination(SqliteTransactionContext& c, const TreeSnapshot& t, NodeId id, const std::string& name) {
    auto valid = validateDirectoryName(name);
    if (!valid) return c.fail(valid.error());
    SqliteNodeRepository nodes(c);
    auto dest = nodes.find(id);
    if (!dest) return dest.error();
    if (!dest.value()) return c.fail(ErrorCode::NotFound);
    if (dest.value()->metadata.spaceId != rootOf(t).spaceId) return c.fail(ErrorCode::CrossSpaceMove);
    auto path = nodes.findActiveDirectoryPath(rootOf(t).spaceId, id);
    if (!path) return path.error();
    auto checked = validateTreeDestination(t, path.value());
    if (!checked) return c.fail(checked.error());
    auto q = detail::query(c, "SELECT 1 FROM nodes WHERE space_id=?1 AND parent_id=?2 AND name=?3 COLLATE BINARY AND trashed=0 AND id<>?4 LIMIT 1", { rootOf(t).spaceId.toString(), id.toString(), name, t.root.toString() });
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r) return c.fail(r.error());
    if (r.value()) return c.fail(ErrorCode::NameConflict);
    return {};
}
Result<void> bumpParent(SqliteTransactionContext& c, NodeId id, UtcTime at) {
    SqliteNodeRepository nodes(c);
    auto n = nodes.find(id);
    if (!n) return n.error();
    if (!n.value()) return c.fail(ErrorCode::IntegrityError);
    auto r = nodes.advanceRevision(id, n.value()->metadata.revision, at);
    if (!r) return r.error();
    return {};
}
Result<NodeDetails> result(SqliteTransactionContext& c, NodeId id) {
    SqliteNodeRepository nodes(c);
    auto n = nodes.find(id);
    if (!n) return n.error();
    if (!n.value()) return c.fail(ErrorCode::IntegrityError);
    return *n.value();
}
Result<void> entryMatches(SqliteTransactionContext& c, const TreeSnapshot& tree, const TrashEntry& e) {
    auto summary = summarizeTree(tree);
    if (!summary) return c.fail(summary.error());
    auto& r = rootOf(tree);
    auto& s = summary.value();
    if (e.rootNodeId != tree.root || e.spaceId != r.spaceId || e.nodeCount != s.total.nodes || e.logicalBytes != s.total.bytes || e.soleCreatorId != s.soleCreator || e.kind != (std::find_if(tree.members.begin(), tree.members.end(), [&](const auto& m) { return m.node.metadata.id == tree.root; })->node.content.index() == 0 ? NodeKind::Directory : NodeKind::File)) return c.fail(ErrorCode::IntegrityError);
    return {};
}
}  // namespace
Result<TreeSnapshot> SqliteTreeRepository::activeSubtree(NodeId id, QueryBudget budget) {
    if (!validateQueryBudget(budget)) return context_.fail(ErrorCode::InvalidArgument);
    detail::QueryGuard guard(context_.connection(), budget);
    SqliteNodeRepository nodes(context_);
    auto root = nodes.find(id);
    if (!root) return context_.fail(guard.error(root.error()));
    if (!root.value()) return context_.fail(ErrorCode::NotFound);
    auto q = detail::query(context_, "WITH RECURSIVE tree(id) AS (SELECT id FROM nodes WHERE id=?1 AND trashed=0 UNION SELECT n.id FROM nodes n JOIN tree t ON n.parent_id=t.id WHERE n.trashed=0 LIMIT ?2) SELECT " + std::string(columns) + " FROM tree t JOIN nodes n ON n.id=t.id LEFT JOIN blobs b ON b.id=n.blob_id ORDER BY n.id", { id.toString(), std::int64_t(std::min(kTreeReadLimit, budget.candidates + 1)) });
    if (!q) return context_.fail(guard.error(q.error()));
    TreeSnapshot out{ id, {} };
    for (;;) {
        auto r = q.value()->step();
        if (!r) return context_.fail(guard.error(r.error()));
        if (!r.value()) break;
        auto m = decode(*q.value());
        if (!m) return context_.fail(m.error());
        out.members.push_back(m.value());
    }
    if (guard.exceeded() || out.members.size() > budget.candidates) return context_.fail(ErrorCode::QueryLimitExceeded);
    return out;
}
Result<TreeSnapshot> SqliteTreeRepository::trashSubtree(TrashId id, QueryBudget budget) {
    if (!validateQueryBudget(budget)) return context_.fail(ErrorCode::InvalidArgument);
    detail::QueryGuard guard(context_.connection(), budget);
    SqliteTrashRepository trash(context_);
    auto entry = trash.find(id);
    if (!entry) return entry.error();
    if (!entry.value()) return context_.fail(ErrorCode::NotFound);
    auto q = detail::query(context_, "SELECT " + std::string(columns) + " FROM nodes n LEFT JOIN blobs b ON b.id=n.blob_id WHERE n.trash_entry_id=?1 ORDER BY n.id LIMIT ?2", { id.toString(), std::int64_t(std::min(kTreeReadLimit, budget.candidates + 1)) });
    if (!q) return context_.fail(guard.error(q.error()));
    TreeSnapshot out{ entry.value()->rootNodeId, {} };
    for (;;) {
        auto r = q.value()->step();
        if (!r) return context_.fail(guard.error(r.error()));
        if (!r.value()) break;
        auto m = decode(*q.value());
        if (!m) return context_.fail(m.error());
        out.members.push_back(m.value());
    }
    if (guard.exceeded() || out.members.size() > budget.candidates) return context_.fail(ErrorCode::QueryLimitExceeded);
    return out;
}
Result<Page<NodeDetails, ChildPosition>> SqliteTreeRepository::search(const Principal& actor, const SearchQuery& query) {
    if (!validateQueryBudget(query.budget)) return context_.fail(ErrorCode::InvalidArgument);
    detail::QueryGuard guard(context_.connection(), query.budget);
    SqliteSpaceRepository spaces(context_);
    auto space = spaces.find(query.spaceId);
    if (!space) return space.error();
    if (!space.value() || (space.value()->kind == SpaceKind::Private && space.value()->ownerId != actor.userId)) return context_.fail(ErrorCode::NotFound);
    // 逐候选字面匹配，候选计数包括不匹配的名称；避免LIKE通配或隐式casefold。
    auto q = detail::query(context_, "SELECT id,name FROM nodes WHERE space_id=?1 AND trashed=0 AND trash_entry_id IS NULL AND parent_id IS NOT NULL AND (?2 IS NULL OR (name COLLATE BINARY,id)>(?2,?3)) ORDER BY name COLLATE BINARY,id LIMIT ?4",
                           { query.spaceId.toString(), query.after ? detail::Parameter(query.after->name) : detail::Parameter(nullptr), query.after ? detail::Parameter(query.after->id.toString()) : detail::Parameter(nullptr), std::int64_t(query.budget.candidates + 1) });
    if (!q) return context_.fail(guard.error(q.error()));
    Page<NodeDetails, ChildPosition> out;
    std::size_t count = 0;
    SqliteNodeRepository nodes(context_);
    for (;;) {
        auto r = q.value()->step();
        if (!r) return context_.fail(guard.error(r.error()));
        if (guard.exceeded()) return context_.fail(ErrorCode::QueryLimitExceeded);
        if (!r.value()) return out;
        if (++count > query.budget.candidates) return context_.fail(ErrorCode::QueryLimitExceeded);
        auto id = readId<NodeId>(*q.value(), 0);
        auto name = q.value()->text(1);
        if (!id || !name) return context_.fail(ErrorCode::IntegrityError);
        if (name.value().find(query.text.value()) == std::string::npos) continue;
        auto n = nodes.find(id.value());
        if (!n) return context_.fail(guard.error(n.error()));
        if (!n.value()) continue;
        if (out.items.size() == static_cast<std::size_t>(query.limit.value())) {
            auto& last = out.items.back().metadata;
            out.next = ChildPosition{ last.name, last.id };
            return out;
        }
        out.items.push_back(*n.value());
    }
}
Result<NodeDetails> SqliteTreeRepository::move(const TreeMutationPlan& plan) {
    auto tree = activeSubtree(plan.expected.root, {});
    if (!tree) return tree.error();
    auto checked = matches(plan.expected, tree.value(), plan.rootCondition);
    if (!checked) return context_.fail(checked.error());
    auto root = rootOf(tree.value());
    if (!root.parentId) return context_.fail(ErrorCode::Forbidden);
    checked = destination(context_, tree.value(), plan.destination, plan.name);
    if (!checked) return checked.error();
    if (root.parentId == plan.destination && root.name == plan.name) return result(context_, root.id);
    auto rev = root.revision.add(1);
    if (!rev) return context_.fail(rev.error());
    auto at = nondecreasingEventTime(plan.occurredAt, root.createdAt, root.updatedAt);
    checked = detail::one(context_, detail::write(context_, "UPDATE nodes SET parent_id=?1,name=?2,updated_at=?3,revision=?4 WHERE id=?5 AND revision=?6 AND trashed=0", { plan.destination.toString(), plan.name, at.time_since_epoch().count(), rev.value().value(), root.id.toString(), root.revision.value() }));
    if (!checked) return checked.error();
    checked = bumpParent(context_, *root.parentId, at);
    if (!checked) return checked.error();
    if (*root.parentId != plan.destination) {
        checked = bumpParent(context_, plan.destination, at);
        if (!checked) return checked.error();
    }
    return result(context_, root.id);
}
Result<void> SqliteTreeRepository::detach(const TreeSnapshot& expected, NodeCondition condition, const TrashEntry& entry) {
    auto tree = activeSubtree(expected.root, {});
    if (!tree) return tree.error();
    auto checked = matches(expected, tree.value(), condition);
    if (!checked) return context_.fail(checked.error());
    auto root = rootOf(tree.value());
    if (!root.parentId) return context_.fail(ErrorCode::Forbidden);
    checked = entryMatches(context_, tree.value(), entry);
    if (!checked) return checked.error();
    auto summary = summarizeTree(tree.value());
    if (!validateTrashEntry(entry) || entry.originalParentId != *root.parentId || entry.originalName != root.name || entry.deletedAt < summary.value().latestEvent) return context_.fail(ErrorCode::IntegrityError);
    for (const auto& m : tree.value().members) {
        auto& n = m.node.metadata;
        auto next = n.revision.add(1);
        if (!next) return context_.fail(next.error());
        checked = detail::one(context_, detail::write(context_, "UPDATE nodes SET trash_entry_id=?1,trashed=1,parent_id=?2,updated_at=?3,revision=?4 WHERE id=?5 AND revision=?6 AND trashed=0", { entry.id.toString(), n.id == root.id ? detail::Parameter(nullptr) : detail::Parameter(n.parentId->toString()), entry.deletedAt.time_since_epoch().count(), next.value().value(), n.id.toString(), n.revision.value() }));
        if (!checked) return checked.error();
    }
    return bumpParent(context_, *root.parentId, entry.deletedAt);
}
Result<NodeDetails> SqliteTreeRepository::restore(const TreeMutationPlan& plan, TrashCondition condition) {
    SqliteTrashRepository trash(context_);
    auto e = trash.find(condition.id);
    if (!e) return e.error();
    if (!e.value()) return context_.fail(ErrorCode::NotFound);
    if (e.value()->revision != condition.revision) return context_.fail(ErrorCode::PreconditionFailed);
    auto tree = trashSubtree(condition.id, {});
    if (!tree) return tree.error();
    auto checked = matches(plan.expected, tree.value(), plan.rootCondition);
    if (!checked) return context_.fail(checked.error());
    checked = entryMatches(context_, tree.value(), *e.value());
    if (!checked) return checked.error();
    checked = destination(context_, tree.value(), plan.destination, plan.name);
    if (!checked) return checked.error();
    auto summary = summarizeTree(tree.value());
    auto at = nondecreasingEventTime(plan.occurredAt, e.value()->deletedAt, summary.value().latestEvent);
    for (const auto& m : tree.value().members) {
        auto& n = m.node.metadata;
        auto rev = n.revision.add(1);
        if (!rev) return context_.fail(rev.error());
        checked = detail::one(context_, detail::write(context_, "UPDATE nodes SET trash_entry_id=NULL,trashed=0,parent_id=?1,name=?2,updated_at=?3,revision=?4 WHERE id=?5 AND revision=?6 AND trash_entry_id=?7", { n.id == tree.value().root ? plan.destination.toString() : n.parentId->toString(), n.id == tree.value().root ? plan.name : n.name, at.time_since_epoch().count(), rev.value().value(), n.id.toString(), n.revision.value(), condition.id.toString() }));
        if (!checked) return checked.error();
    }
    checked = bumpParent(context_, plan.destination, at);
    if (!checked) return checked.error();
    return result(context_, tree.value().root);
}
Result<void> SqliteTreeRepository::eraseMembers(TrashCondition condition, const TreeSnapshot& expected) {
    SqliteTrashRepository trash(context_);
    auto e = trash.find(condition.id);
    if (!e) return e.error();
    if (!e.value()) return context_.fail(ErrorCode::NotFound);
    if (e.value()->revision != condition.revision) return context_.fail(ErrorCode::PreconditionFailed);
    auto actual = trashSubtree(condition.id, {});
    if (!actual) return actual.error();
    if (!summarizeTree(expected)) return context_.fail(ErrorCode::InvalidArgument);
    auto checked = matches(expected, actual.value(), { expected.root, rootOf(expected).revision });
    if (!checked) return context_.fail(checked.error());
    checked = entryMatches(context_, actual.value(), *e.value());
    if (!checked) return checked.error();
    auto n = detail::write(context_, "DELETE FROM nodes WHERE trash_entry_id=?1", { condition.id.toString() });
    if (!n) return n.error();
    if (n.value() != static_cast<std::int64_t>(actual.value().members.size())) return context_.fail(ErrorCode::IntegrityError);
    return {};
}
}  // namespace anof::infrastructure::sqlite
