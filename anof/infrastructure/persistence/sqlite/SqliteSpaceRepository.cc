#include <anof/infrastructure/persistence/sqlite/SqliteSpaceRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>

namespace anof::infrastructure::sqlite {

Result<std::optional<Space>> SqliteSpaceRepository::find(SpaceId id) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    auto prepared = SqliteStatement::prepare(context_.connection(),
        "SELECT id, kind, owner_id, root_node_id FROM spaces WHERE id=?1");
    if (!prepared) return context_.fail(prepared.error());
    auto& statement = *prepared.value();
    auto bound = statement.bindText(1, id.toString());
    if (!bound) return context_.fail(bound.error());
    auto row = statement.step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return std::optional<Space>{};
    auto storedId = readId<SpaceId>(statement, 0);
    auto kind = readSpaceKind(statement, 1);
    auto ownerNull = statement.isNull(2);
    auto root = readId<NodeId>(statement, 3);
    if (!storedId || !kind || !ownerNull || !root || storedId.value() != id) return context_.fail(ErrorCode::IntegrityError);
    std::optional<UserId> owner;
    if (!ownerNull.value()) {
        auto decoded = readId<UserId>(statement, 2);
        if (!decoded) return context_.fail(decoded.error());
        owner = decoded.value();
    }
    if ((kind.value() == SpaceKind::Private) != owner.has_value()) return context_.fail(ErrorCode::IntegrityError);
    Space space{ storedId.value(), kind.value(), owner, root.value() };
    row = statement.step();
    if (!row) return context_.fail(row.error());
    if (row.value()) return context_.fail(ErrorCode::IntegrityError);
    return std::optional<Space>{ std::move(space) };
}

Result<std::optional<Space>> SqliteSpaceRepository::findPrivate(UserId user) {
    auto query = detail::query(context_, "SELECT id FROM spaces WHERE kind=0 AND owner_id=?1 LIMIT 2", { user.toString() });
    if (!query) return query.error();
    auto row = query.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return std::optional<Space>{};
    auto id = readId<SpaceId>(*query.value(), 0);
    if (!id) return context_.fail(id.error());
    row = query.value()->step();
    if (!row) return context_.fail(row.error());
    if (row.value()) return context_.fail(ErrorCode::IntegrityError);
    auto space = find(id.value());
    if (!space) return space.error();
    if (!space.value() || space.value()->kind != SpaceKind::Private || space.value()->ownerId != user)
        return context_.fail(ErrorCode::IntegrityError);
    return space;
}

Result<std::vector<Space>> SqliteSpaceRepository::listAccessible(UserId user) {
    auto query = detail::query(context_, "SELECT id FROM spaces WHERE kind=1 OR (kind=0 AND owner_id=?1) ORDER BY id LIMIT 3", { user.toString() });
    if (!query) return query.error();
    std::vector<Space> result;
    bool personal = false, common = false;
    for (;;) {
        auto row = query.value()->step();
        if (!row) return context_.fail(row.error());
        if (!row.value()) break;
        auto id = readId<SpaceId>(*query.value(), 0);
        if (!id) return context_.fail(id.error());
        auto space = find(id.value());
        if (!space) return space.error();
        if (!space.value()) return context_.fail(ErrorCode::IntegrityError);
        if (space.value()->kind == SpaceKind::Private) {
            if (personal || space.value()->ownerId != user) return context_.fail(ErrorCode::IntegrityError);
            personal = true;
        } else {
            if (common) return context_.fail(ErrorCode::IntegrityError);
            common = true;
        }
        result.push_back(*space.value());
    }
    if (!personal || !common || result.size() != 2) return context_.fail(ErrorCode::IntegrityError);
    return result;
}

Result<void> SqliteSpaceRepository::insert(const Space& space) {
    if ((space.kind != SpaceKind::Private && space.kind != SpaceKind::Public) ||
        (space.kind == SpaceKind::Private) != space.ownerId.has_value()) return context_.fail(ErrorCode::InvalidArgument);
    const auto owner = space.ownerId ? space.ownerId->toString() : std::string{};
    return detail::one(context_, detail::write(context_,
        "INSERT INTO spaces(id,kind,owner_id,root_node_id) VALUES(?1,?2,?3,?4)",
        { space.id.toString(), std::int64_t(space.kind == SpaceKind::Public ? 1 : 0),
          space.ownerId ? detail::Parameter(owner) : detail::Parameter(nullptr), space.rootNodeId.toString() }));
}

}  // namespace anof::infrastructure::sqlite
