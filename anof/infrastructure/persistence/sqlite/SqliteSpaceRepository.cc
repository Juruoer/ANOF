#include <anof/infrastructure/persistence/sqlite/SqliteSpaceRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

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

}  // namespace anof::infrastructure::sqlite
