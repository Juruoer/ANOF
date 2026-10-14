#include <anof/infrastructure/persistence/sqlite/SqliteSettingsRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

namespace anof::infrastructure::sqlite {
Result<std::optional<InstanceSettings>> SqliteSettingsRepository::get() {
    auto q = detail::query(context_, "SELECT instance_id,bootstrap_operation_id,initialized_at,max_users,default_private_bytes,default_private_nodes,default_contribution_bytes,default_contribution_nodes,revision FROM instance_settings WHERE singleton=1");
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return std::optional<InstanceSettings>{};
    auto& s = *q.value();
    auto instance = readId<InstanceId>(s, 0);
    auto operation = readId<OperationId>(s, 1);
    auto time = readTime(s, 2);
    auto users = readQuantity<UserLimit>(s, 3);
    auto pb = readQuantity<ByteCount>(s, 4);
    auto pn = readQuantity<NodeCount>(s, 5);
    auto cb = readQuantity<ByteCount>(s, 6);
    auto cn = readQuantity<NodeCount>(s, 7);
    auto revision = readQuantity<Revision>(s, 8);
    if (!instance || !operation || !time || !users || !pb || !pn || !cb || !cn || !revision || pn.value().value() < 1 || cn.value().value() < 1) return context_.fail(ErrorCode::IntegrityError);
    return std::optional<InstanceSettings>{InstanceSettings{ instance.value(), operation.value(), time.value(), users.value(), {pb.value(), pn.value() }, {cb.value(), cn.value() }, revision.value() } };
}

Result<void> SqliteSettingsRepository::insertBootstrap(const InstanceSettings& s) {
    auto existing = get();
    if (!existing) return existing.error();
    if (existing.value()) return context_.fail(ErrorCode::Conflict);
    if (s.revision.value() != 1 || s.defaultPrivate.nodes.value() < 1 || s.defaultContribution.nodes.value() < 1) return context_.fail(ErrorCode::InvalidArgument);
    return detail::one(context_, detail::write(context_, "INSERT INTO instance_settings VALUES(1,?1,?2,?3,?4,?5,?6,?7,?8,1)", {s.instanceId.toString(), s.bootstrapOperationId.toString(), s.initializedAt.time_since_epoch().count(), s.maxUsers.value(), s.defaultPrivate.bytes.value(), s.defaultPrivate.nodes.value(), s.defaultContribution.bytes.value(), s.defaultContribution.nodes.value() }));
}
}  // namespace anof::infrastructure::sqlite
