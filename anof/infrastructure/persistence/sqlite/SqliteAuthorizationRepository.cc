#include <anof/infrastructure/persistence/sqlite/SqliteAuthorizationRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

namespace anof::infrastructure::sqlite {

Result<Principal> SqliteAuthorizationRepository::requireActiveSession(const SessionDigest& digest, UtcTime now) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    // 不把状态条件放入 WHERE，避免损坏的枚举、epoch 或时间被误认成普通失效凭据。
    auto prepared = SqliteStatement::prepare(context_.connection(),
        "SELECT s.user_id, s.auth_epoch, s.expires_at, s.revoked, u.id, u.role, u.enabled, u.auth_epoch "
        "FROM sessions AS s LEFT JOIN users AS u ON u.id=s.user_id WHERE s.token_hash=?1");
    if (!prepared) return context_.fail(prepared.error());
    auto& statement = *prepared.value();
    auto bound = statement.bindBlob(1, digest.bytes().data(), digest.bytes().size());
    if (!bound) return context_.fail(bound.error());
    auto row = statement.step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return context_.fail(ErrorCode::Unauthenticated);
    auto sessionUser = readId<UserId>(statement, 0);
    auto sessionEpoch = readQuantity<AuthEpoch>(statement, 1);
    auto expiresAt = readTime(statement, 2);
    auto revoked = readBoolean(statement, 3);
    auto user = readId<UserId>(statement, 4);
    auto role = readUserRole(statement, 5);
    auto enabled = readBoolean(statement, 6);
    auto epoch = readQuantity<AuthEpoch>(statement, 7);
    if (!sessionUser || !sessionEpoch || !expiresAt || !revoked || !user || !role || !enabled || !epoch) {
        return context_.fail(ErrorCode::IntegrityError);
    }
    if (sessionUser.value() != user.value()) return context_.fail(ErrorCode::IntegrityError);
    const Principal principal{ user.value(), role.value(), epoch.value() };
    row = statement.step();
    if (!row) return context_.fail(row.error());
    if (row.value()) return context_.fail(ErrorCode::IntegrityError);
    if (revoked.value() || !enabled.value() || expiresAt.value() <= now || sessionEpoch.value() != epoch.value()) {
        return context_.fail(ErrorCode::Unauthenticated);
    }
    return principal;
}

}  // namespace anof::infrastructure::sqlite
