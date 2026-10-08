#include <anof/infrastructure/persistence/sqlite/SqliteAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>

#include <sqlite3.h>

namespace anof::infrastructure::sqlite {

Result<void> SqliteAuditRepository::append(const DirectoryCreatedEvent& event) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    auto prepared = SqliteStatement::prepare(context_.connection(),
        "INSERT INTO audit_events(actor_id, space_id, node_id, parent_id, request_id, action, occurred_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5, 'directory_created', ?6)");
    if (!prepared) return context_.fail(prepared.error());
    auto& statement = *prepared.value();
    auto bound = statement.bindText(1, event.actor.toString());
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindText(2, event.spaceId.toString());
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindText(3, event.nodeId.toString());
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindText(4, event.parentId.toString());
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindText(5, event.requestId.toString());
    if (!bound) return context_.fail(bound.error());
    bound = statement.bindInt64(6, event.occurredAt.time_since_epoch().count());
    if (!bound) return context_.fail(bound.error());
    auto row = statement.step();
    if (!row) return context_.fail(row.error());
    if (row.value() || sqlite3_changes(context_.connection().handle()) != 1) return context_.fail(ErrorCode::IntegrityError);
    return {};
}

}  // namespace anof::infrastructure::sqlite
