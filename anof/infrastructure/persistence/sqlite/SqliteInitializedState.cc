#include <anof/infrastructure/persistence/sqlite/SqliteInitializedState.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSettingsRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUserRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
namespace anof::infrastructure::sqlite {
namespace detail {
// 业务 Store 的数据库完整性门禁；文件发布和实例配置匹配由维护入口负责。
Result<InitReceipt> verifyInitialized(SqliteConnection& connection, PasswordHashLimits limits, bool initial) {
    bool active = false;
    SqliteTransactionContext context(connection, active);
    auto begun = context.beginRead();
    if (!begun) return begun.error();
    SqliteSettingsRepository settings(context);
    auto stored = settings.get();
    if (!stored) return stored.error();
    if (!stored.value()) return ErrorCode::NotFound;
    auto check = detail::query(context,
        "SELECT (SELECT count(*) FROM users WHERE role=1 AND enabled=1)=1 "
        "AND (SELECT count(*) FROM spaces WHERE kind=1)=1 "
        "AND (SELECT count(*) FROM users)<=?1 "
        "AND NOT EXISTS(SELECT 1 FROM users u WHERE NOT EXISTS(SELECT 1 FROM spaces s WHERE s.kind=0 AND s.owner_id=u.id)) "
        "AND NOT EXISTS(SELECT 1 FROM spaces s LEFT JOIN nodes n ON n.id=s.root_node_id LEFT JOIN space_quotas q ON q.space_id=s.id "
        "WHERE n.id IS NULL OR n.space_id<>s.id OR n.parent_id IS NOT NULL OR n.name<>'' OR n.kind<>0 OR n.trashed<>0 OR n.blob_id IS NOT NULL "
        "OR q.space_id IS NULL OR q.used_nodes<1 OR (s.kind=0 AND n.created_by<>s.owner_id) "
        "OR (s.kind=1 AND n.created_by<>(SELECT id FROM users WHERE role=1))) "
        "AND NOT EXISTS(SELECT 1 FROM users u CROSS JOIN spaces s LEFT JOIN public_contribution_quotas q ON q.space_id=s.id AND q.user_id=u.id "
        "WHERE s.kind=1 AND (q.user_id IS NULL OR (u.role=1 AND q.used_nodes<1)))",
        { stored.value()->maxUsers.value() });
    if (!check) return check.error();
    auto row = check.value()->step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::IntegrityError;
    auto valid = readBoolean(*check.value(), 0);
    if (!valid || !valid.value()) return ErrorCode::IntegrityError;
    SqliteUserRepository users(context, limits);
    auto ids = detail::query(context, "SELECT id FROM users");
    if (!ids) return ids.error();
    for (;;) {
        row = ids.value()->step();
        if (!row) return row.error();
        if (!row.value()) break;
        auto id = readId<UserId>(*ids.value(), 0);
        if (!id) return id.error();
        auto password = users.findPasswordById(id.value());
        if (!password) return password.error();
        if (!password.value()) return ErrorCode::IntegrityError;
    }
    auto receipt = detail::query(context,
        "SELECT u.id,p.id,s.id FROM users u JOIN spaces p ON p.kind=1 JOIN spaces s ON s.kind=0 AND s.owner_id=u.id "
        "WHERE u.role=1 AND (SELECT count(*) FROM identity_audit_events a WHERE a.action='instance_initialized')=1 "
        "AND EXISTS(SELECT 1 FROM identity_audit_events a WHERE a.action='instance_initialized' AND a.subject_user_id=u.id "
        "AND a.instance_id=?1 AND a.occurred_at=?2)",
        { stored.value()->instanceId.toString(), stored.value()->initializedAt.time_since_epoch().count() });
    if (!receipt) return receipt.error();
    row = receipt.value()->step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::IntegrityError;
    auto admin = readId<UserId>(*receipt.value(), 0);
    auto common = readId<SpaceId>(*receipt.value(), 1);
    auto personal = readId<SpaceId>(*receipt.value(), 2);
    if (!admin || !common || !personal) return ErrorCode::IntegrityError;
    if (initial) {
        auto exact = detail::query(context,
            "SELECT (SELECT count(*) FROM users)=1 AND (SELECT count(*) FROM spaces)=2 "
            "AND (SELECT count(*) FROM nodes)=2 AND (SELECT count(*) FROM space_quotas)=2 "
            "AND (SELECT count(*) FROM public_contribution_quotas)=1 "
            "AND NOT EXISTS(SELECT 1 FROM sessions) AND NOT EXISTS(SELECT 1 FROM invites) "
            "AND NOT EXISTS(SELECT 1 FROM blobs) AND NOT EXISTS(SELECT 1 FROM audit_events) "
            "AND (SELECT count(*) FROM identity_audit_events)=1 "
            "AND NOT EXISTS(SELECT 1 FROM space_quotas WHERE used_bytes<>0 OR used_nodes<>1 OR reserved_bytes<>0 OR reserved_nodes<>0 OR revision<>1) "
            "AND NOT EXISTS(SELECT 1 FROM public_contribution_quotas WHERE used_bytes<>0 OR used_nodes<>1 OR reserved_bytes<>0 OR reserved_nodes<>0 OR revision<>1) "
            "AND NOT EXISTS(SELECT 1 FROM users WHERE auth_epoch<>0 OR password_version<>1 OR revision<>1 OR created_at<>?1 OR updated_at<>?1) "
            "AND NOT EXISTS(SELECT 1 FROM nodes WHERE revision<>1 OR created_at<>?1 OR updated_at<>?1) "
            "AND NOT EXISTS(SELECT 1 FROM instance_settings WHERE revision<>1) "
            "AND EXISTS(SELECT 1 FROM space_quotas q JOIN spaces s ON s.id=q.space_id CROSS JOIN instance_settings i "
            "WHERE s.kind=0 AND q.limit_bytes=i.default_private_bytes AND q.limit_nodes=i.default_private_nodes) "
            "AND EXISTS(SELECT 1 FROM public_contribution_quotas q CROSS JOIN instance_settings i "
            "WHERE q.limit_bytes=i.default_contribution_bytes AND q.limit_nodes=i.default_contribution_nodes)",
            { stored.value()->initializedAt.time_since_epoch().count() });
        if (!exact) return exact.error();
        auto present = exact.value()->step();
        if (!present) return present.error();
        if (!present.value()) return ErrorCode::IntegrityError;
        auto validInitial = readBoolean(*exact.value(), 0);
        if (!validInitial || !validInitial.value()) return ErrorCode::IntegrityError;
    }
    return InitReceipt{ { stored.value()->instanceId, stored.value()->bootstrapOperationId }, admin.value(), common.value(), personal.value() };
}
}  // namespace

}
