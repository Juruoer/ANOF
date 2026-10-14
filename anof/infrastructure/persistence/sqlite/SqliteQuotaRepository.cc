#include <anof/infrastructure/persistence/sqlite/SqliteQuotaRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>

namespace anof::infrastructure::sqlite {
namespace {
struct Scope {
    explicit Scope(const QuotaScope& scope) {
        if (const auto* contribution = std::get_if<PublicContributionQuota>(&scope)) {
            space = contribution->spaceId.toString();
            user = contribution->userId.toString();
            table = "public_contribution_quotas";
        } else {
            space = std::get<SpaceQuota>(scope).spaceId.toString();
            table = "space_quotas";
        }
    }
    detail::Parameter owner() const { return user.empty() ? detail::Parameter(nullptr) : detail::Parameter(user); }
    std::string condition() const { return user.empty() ? "space_id=?1 AND ?2 IS NULL" : "space_id=?1 AND user_id=?2"; }
    std::string space, user, table;
};
Result<void> validate(const QuotaState& state) {
    auto bytes = state.used.bytes.add(state.reserved.bytes.value());
    auto nodes = state.used.nodes.add(state.reserved.nodes.value());
    if (!bytes || !nodes || bytes.value().value() > state.limit.bytes.value() || nodes.value().value() > state.limit.nodes.value()) return ErrorCode::IntegrityError;
    return {};
}
}  // namespace

Result<QuotaSnapshot> SqliteQuotaRepository::get(const QuotaScope& scope) {
    const Scope key(scope);
    auto query = detail::query(context_, "SELECT limit_bytes,limit_nodes,used_bytes,used_nodes,reserved_bytes,reserved_nodes,revision,s.kind FROM " + key.table +
        " q JOIN spaces s ON s.id=q.space_id WHERE " + key.condition(), { key.space, key.owner() });
    if (!query) return query.error();
    auto row = query.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return context_.fail(ErrorCode::IntegrityError);
    auto& statement = *query.value();
    auto lb = readQuantity<ByteCount>(statement, 0);
    auto ln = readQuantity<NodeCount>(statement, 1);
    auto ub = readQuantity<ByteCount>(statement, 2);
    auto un = readQuantity<NodeCount>(statement, 3);
    auto rb = readQuantity<ByteCount>(statement, 4);
    auto rn = readQuantity<NodeCount>(statement, 5);
    auto revision = readQuantity<Revision>(statement, 6);
    auto kind = readSpaceKind(statement, 7);
    if (!lb || !ln || !ub || !un || !rb || !rn || !revision || !kind || (!key.user.empty() && kind.value() != SpaceKind::Public)) return context_.fail(ErrorCode::IntegrityError);
    QuotaSnapshot result{ { { lb.value(), ln.value() }, { ub.value(), un.value() }, { rb.value(), rn.value() } }, revision.value() };
    auto valid = validate(result.state);
    if (!valid) return context_.fail(valid.error());
    return result;
}

Result<void> SqliteQuotaRepository::insert(const QuotaScope& scope, const QuotaSnapshot& quota) {
    auto valid = validate(quota.state);
    if (!valid || quota.revision.value() != 1) return context_.fail(ErrorCode::InvalidArgument);
    const Scope key(scope);
    const auto& q = quota.state;
    const auto sql = "INSERT INTO " + key.table + "(space_id," + (key.user.empty() ? std::string{} : "user_id,") +
        "limit_bytes,limit_nodes,used_bytes,used_nodes,reserved_bytes,reserved_nodes,revision) SELECT ?1," +
        (key.user.empty() ? std::string{} : "?2,") + "?3,?4,?5,?6,?7,?8,1" + (key.user.empty() ? " WHERE ?2 IS NULL" : "");
    return detail::one(context_, detail::write(context_, sql, { key.space, key.owner(), q.limit.bytes.value(), q.limit.nodes.value(),
        q.used.bytes.value(), q.used.nodes.value(), q.reserved.bytes.value(), q.reserved.nodes.value() }));
}

Result<void> SqliteQuotaRepository::update(const QuotaScope& scope, const QuotaSnapshot& quota) {
    const Scope key(scope);
    const auto& q = quota.state;
    return detail::one(context_, detail::write(context_, "UPDATE " + key.table +
        " SET limit_bytes=?3,limit_nodes=?4,used_bytes=?5,used_nodes=?6,revision=?7 WHERE " + key.condition() + " AND revision=?8",
        { key.space, key.owner(), q.limit.bytes.value(), q.limit.nodes.value(), q.used.bytes.value(), q.used.nodes.value(), quota.revision.value(), quota.revision.value() - 1 }));
}

Result<QuotaSnapshot> SqliteQuotaRepository::setLimit(const QuotaScope& scope, Revision expected, QuotaDelta limit) {
    auto current = get(scope);
    if (!current) return current.error();
    auto quota = current.value();
    if (quota.revision != expected) return context_.fail(ErrorCode::PreconditionFailed);
    if (quota.state.limit.bytes == limit.bytes && quota.state.limit.nodes == limit.nodes) return quota;
    quota.state.limit = limit;
    if (!validate(quota.state)) return context_.fail(ErrorCode::QuotaExceeded);
    auto next = quota.revision.add(1);
    if (!next) return context_.fail(next.error());
    quota.revision = next.value();
    auto written = update(scope, quota);
    if (!written) return written.error();
    return quota;
}

Result<void> SqliteQuotaRepository::chargeUsed(const QuotaScope& scope, QuotaDelta delta) {
    auto current = get(scope);
    if (!current) return current.error();
    auto charged = anof::chargeUsed(current.value().state, delta);
    if (!charged) return context_.fail(charged.error());
    if (delta.bytes.value() == 0 && delta.nodes.value() == 0) return {};
    auto next = current.value().revision.add(1);
    if (!next) return context_.fail(next.error());
    return update(scope, { charged.value(), next.value() });
}
}  // namespace anof::infrastructure::sqlite
