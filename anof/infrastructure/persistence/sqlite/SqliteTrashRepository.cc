#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSpaceRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTrashRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
namespace anof::infrastructure::sqlite {
namespace {
constexpr const char* fields = "id,space_id,root_node_id,original_parent_id,original_name,kind,deleted_by,deleted_at,expires_at,revision,node_count,logical_bytes,sole_creator_id";
Result<TrashEntry> decode(SqliteStatement& s) {
    auto id = readId<TrashId>(s, 0);
    auto space = readId<SpaceId>(s, 1);
    auto root = readId<NodeId>(s, 2);
    auto parent = readId<NodeId>(s, 3);
    auto name = s.text(4);
    auto kind = s.integer(5);
    auto user = readId<UserId>(s, 6);
    auto time = readTime(s, 7);
    auto expires = readTime(s, 8);
    auto rev = readQuantity<Revision>(s, 9);
    auto count = readQuantity<NodeCount>(s, 10);
    auto bytes = readQuantity<ByteCount>(s, 11);
    auto nil = s.isNull(12);
    if (!id || !space || !root || !parent || !name || !kind || kind.value() < 0 || kind.value() > 1 || !user || !time || !expires || !rev || !count || !bytes || !nil) return ErrorCode::IntegrityError;
    std::optional<UserId> sole;
    if (!nil.value()) {
        auto v = readId<UserId>(s, 12);
        if (!v) return v.error();
        sole = v.value();
    }
    TrashEntry e{ id.value(), space.value(), root.value(), parent.value(), name.value(), static_cast<NodeKind>(kind.value()), user.value(), time.value(), expires.value(), rev.value(), count.value(), bytes.value(), sole };
    if (!validateTrashEntry(e)) return ErrorCode::IntegrityError;
    return e;
}
}  // namespace
Result<std::optional<TrashEntry>> SqliteTrashRepository::find(TrashId id) {
    auto q = detail::query(context_, "SELECT " + std::string(fields) + " FROM trash_entries WHERE id=?1", { id.toString() });
    if (!q) return q.error();
    auto r = q.value()->step();
    if (!r) return context_.fail(r.error());
    if (!r.value()) return std::optional<TrashEntry>{};
    auto e = decode(*q.value());
    if (!e) return context_.fail(e.error());
    return std::optional<TrashEntry>{ e.value() };
}
Result<void> SqliteTrashRepository::insert(const TrashEntry& e) {
    if (!validateTrashEntry(e) || e.revision.value() != 1) return context_.fail(ErrorCode::InvalidArgument);
    return detail::one(context_, detail::write(context_, "INSERT INTO trash_entries(" + std::string(fields) + ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)",
                                               { e.id.toString(), e.spaceId.toString(), e.rootNodeId.toString(), e.originalParentId.toString(), e.originalName, std::int64_t(e.kind), e.deletedBy.toString(), e.deletedAt.time_since_epoch().count(), e.expiresAt.time_since_epoch().count(), e.revision.value(), e.nodeCount.value(), e.logicalBytes.value(), e.soleCreatorId ? detail::Parameter(e.soleCreatorId->toString()) : detail::Parameter(nullptr) }));
}
Result<void> SqliteTrashRepository::erase(TrashCondition condition) {
    auto e = find(condition.id);
    if (!e) return e.error();
    if (!e.value()) return context_.fail(ErrorCode::NotFound);
    if (e.value()->revision != condition.revision) return context_.fail(ErrorCode::PreconditionFailed);
    return detail::one(context_, detail::write(context_, "DELETE FROM trash_entries WHERE id=?1 AND revision=?2", { condition.id.toString(), condition.revision.value() }));
}
Result<Page<TrashEntry, TrashPosition>> SqliteTrashRepository::list(const TrashQuery& query) {
    SqliteSpaceRepository spaces(context_);
    auto space = spaces.find(query.spaceId);
    if (!space) return space.error();
    if (!space.value() || (space.value()->kind == SpaceKind::Private && space.value()->ownerId != query.actor.userId)) return context_.fail(ErrorCode::NotFound);
    const bool own = space.value()->kind == SpaceKind::Public && query.actor.role != UserRole::Administrator;
    auto q = detail::query(context_, "SELECT " + std::string(fields) + " FROM trash_entries WHERE space_id=?1 AND (?2=0 OR sole_creator_id=?3) AND (?4 IS NULL OR root_node_id=?4) AND (?5 IS NULL OR (deleted_at,id)>(?5,?6)) ORDER BY deleted_at,id LIMIT ?7",
                           { query.spaceId.toString(), std::int64_t(own), query.actor.userId.toString(), query.root ? detail::Parameter(query.root->toString()) : detail::Parameter(nullptr), query.after ? detail::Parameter(query.after->deletedAt.time_since_epoch().count()) : detail::Parameter(nullptr), query.after ? detail::Parameter(query.after->id.toString()) : detail::Parameter(nullptr), query.limit.value() + 1 });
    if (!q) return q.error();
    Page<TrashEntry, TrashPosition> out;
    for (;;) {
        auto r = q.value()->step();
        if (!r) return context_.fail(r.error());
        if (!r.value()) return out;
        auto e = decode(*q.value());
        if (!e) return context_.fail(e.error());
        if (out.items.size() == static_cast<std::size_t>(query.limit.value())) {
            auto& last = out.items.back();
            out.next = TrashPosition{ last.deletedAt, last.id };
            return out;
        }
        out.items.push_back(e.value());
    }
}
Result<Page<TrashEntry, TrashExpiryPosition>> SqliteTrashRepository::expired(UtcTime now, PageSize size, std::optional<TrashExpiryPosition> after) {
    if (size.value() > 16) return context_.fail(ErrorCode::InvalidArgument);
    auto q = detail::query(context_, "SELECT " + std::string(fields) + " FROM trash_entries WHERE expires_at<=?1 AND (?2 IS NULL OR (expires_at,id)>(?2,?3)) ORDER BY expires_at,id LIMIT ?4",
                           { now.time_since_epoch().count(), after ? detail::Parameter(after->expiresAt.time_since_epoch().count()) : detail::Parameter(nullptr), after ? detail::Parameter(after->id.toString()) : detail::Parameter(nullptr), size.value() + 1 });
    if (!q) return q.error();
    Page<TrashEntry, TrashExpiryPosition> out;
    for (;;) {
        auto r = q.value()->step();
        if (!r) return context_.fail(r.error());
        if (!r.value()) return out;
        auto e = decode(*q.value());
        if (!e) return context_.fail(e.error());
        if (out.items.size() == static_cast<std::size_t>(size.value())) {
            auto& last = out.items.back();
            out.next = TrashExpiryPosition{ last.expiresAt, last.id };
            return out;
        }
        out.items.push_back(e.value());
    }
}
}  // namespace anof::infrastructure::sqlite
