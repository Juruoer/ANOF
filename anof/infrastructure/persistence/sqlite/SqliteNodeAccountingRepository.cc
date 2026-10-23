#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteNodeAccountingRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
namespace anof::infrastructure::sqlite {
Result<NodeAccounting> SqliteNodeAccountingRepository::get() {
    auto q = detail::query(context_, "SELECT used_nodes,reserved_nodes,revision FROM node_accounting WHERE singleton=1");
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return context_.fail(ErrorCode::IntegrityError);
    auto u = readQuantity<NodeCount>(*q.value(), 0);
    auto r = readQuantity<NodeCount>(*q.value(), 1);
    auto v = readQuantity<Revision>(*q.value(), 2);
    if (!u || !r || !v) return context_.fail(ErrorCode::IntegrityError);
    NodeAccounting n{ u.value(), r.value(), v.value() };
    if (!validateNodeAccounting(n)) return context_.fail(ErrorCode::IntegrityError);
    return n;
}
namespace {
Result<void> change(SqliteTransactionContext& c, Result<NodeAccounting> before, NodeCount count, Result<NodeAccounting> (*fn)(NodeAccounting, NodeCount)) {
    if (!before) return before.error();
    auto after = fn(before.value(), count);
    if (!after) return c.fail(after.error());
    if (after.value().revision == before.value().revision) return {};
    auto n = after.value();
    return detail::one(c, detail::write(c, "UPDATE node_accounting SET used_nodes=?1,reserved_nodes=?2,revision=?3 WHERE singleton=1 AND revision=?4", { n.used.value(), n.reserved.value(), n.revision.value(), before.value().revision.value() }));
}
}  // namespace
Result<void> SqliteNodeAccountingRepository::chargeUsed(NodeCount n) { return change(context_, get(), n, chargeNodes); }
Result<void> SqliteNodeAccountingRepository::releaseUsed(NodeCount n) { return change(context_, get(), n, releaseUsedNodes); }
Result<void> SqliteNodeAccountingRepository::reserve(NodeCount n) { return change(context_, get(), n, reserveNodes); }
Result<void> SqliteNodeAccountingRepository::commitReservation(NodeCount n) { return change(context_, get(), n, publishNodes); }
Result<void> SqliteNodeAccountingRepository::releaseReservation(NodeCount n) { return change(context_, get(), n, releaseReservedNodes); }
}  // namespace anof::infrastructure::sqlite
