#include <anof/infrastructure/persistence/sqlite/SqliteQuotaRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

#include <sqlite3.h>

namespace anof::infrastructure::sqlite {

Result<void> SqliteQuotaRepository::chargeUsed(const QuotaScope& scope, QuotaDelta delta) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    const auto* contribution = std::get_if<PublicContributionQuota>(&scope);
    const SpaceId space = contribution ? contribution->spaceId : std::get<SpaceQuota>(scope).spaceId;
    auto prepared = SqliteStatement::prepare(context_.connection(), contribution ?
        "SELECT q.limit_bytes, q.limit_nodes, q.used_bytes, q.used_nodes, q.reserved_bytes, q.reserved_nodes, s.kind "
        "FROM public_contribution_quotas q JOIN spaces s ON s.id=q.space_id WHERE q.space_id=?1 AND q.user_id=?2" :
        "SELECT q.limit_bytes, q.limit_nodes, q.used_bytes, q.used_nodes, q.reserved_bytes, q.reserved_nodes, s.kind "
        "FROM space_quotas q JOIN spaces s ON s.id=q.space_id WHERE q.space_id=?1");
    if (!prepared) return context_.fail(prepared.error());
    auto& statement = *prepared.value();
    auto bound = statement.bindText(1, space.toString());
    if (!bound) return context_.fail(bound.error());
    if (contribution) {
        bound = statement.bindText(2, contribution->userId.toString());
        if (!bound) return context_.fail(bound.error());
    }
    auto row = statement.step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return context_.fail(ErrorCode::IntegrityError);
    auto limitBytes = readQuantity<ByteCount>(statement, 0);
    auto limitNodes = readQuantity<NodeCount>(statement, 1);
    auto usedBytes = readQuantity<ByteCount>(statement, 2);
    auto usedNodes = readQuantity<NodeCount>(statement, 3);
    auto reservedBytes = readQuantity<ByteCount>(statement, 4);
    auto reservedNodes = readQuantity<NodeCount>(statement, 5);
    auto kind = readSpaceKind(statement, 6);
    if (!limitBytes || !limitNodes || !usedBytes || !usedNodes || !reservedBytes || !reservedNodes || !kind) {
        return context_.fail(ErrorCode::IntegrityError);
    }
    if (contribution && kind.value() != SpaceKind::Public) return context_.fail(ErrorCode::IntegrityError);
    const QuotaState state{ { limitBytes.value(), limitNodes.value() }, { usedBytes.value(), usedNodes.value() },
                            { reservedBytes.value(), reservedNodes.value() } };
    auto charged = anof::chargeUsed(state, delta);
    if (!charged) return context_.fail(charged.error());
    row = statement.step();
    if (!row) return context_.fail(row.error());
    if (row.value()) return context_.fail(ErrorCode::IntegrityError);
    prepared.value().reset();
    // BEGIN IMMEDIATE 已取得写事务；检查和两个维度的写回共享连接，不在 SQL 中求可能溢出的和。
    auto update = SqliteStatement::prepare(context_.connection(), contribution ?
        "UPDATE public_contribution_quotas SET used_bytes=?1, used_nodes=?2 WHERE space_id=?3 AND user_id=?4" :
        "UPDATE space_quotas SET used_bytes=?1, used_nodes=?2 WHERE space_id=?3");
    if (!update) return context_.fail(update.error());
    bound = update.value()->bindInt64(1, charged.value().used.bytes.value());
    if (!bound) return context_.fail(bound.error());
    bound = update.value()->bindInt64(2, charged.value().used.nodes.value());
    if (!bound) return context_.fail(bound.error());
    bound = update.value()->bindText(3, space.toString());
    if (!bound) return context_.fail(bound.error());
    if (contribution) {
        bound = update.value()->bindText(4, contribution->userId.toString());
        if (!bound) return context_.fail(bound.error());
    }
    row = update.value()->step();
    if (!row) return context_.fail(row.error());
    if (row.value() || sqlite3_changes(context_.connection().handle()) != 1) return context_.fail(ErrorCode::IntegrityError);
    return {};
}

}  // namespace anof::infrastructure::sqlite
