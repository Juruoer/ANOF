#include <anof/infrastructure/persistence/sqlite/SqliteSessionRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

namespace anof::infrastructure::sqlite {
Result<SessionCount> SqliteSessionRepository::countActive(UserId user, UtcTime now) {
    auto q = detail::query(context_, "SELECT count(*) FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.user_id=?1 AND s.revoked=0 AND s.expires_at>?2 AND s.auth_epoch=u.auth_epoch AND s.credential_mode='bearer'", {user.toString(), now.time_since_epoch().count() });
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return context_.fail(ErrorCode::IntegrityError);
    auto count = readQuantity<SessionCount>(*q.value(), 0);
    if (!count) return context_.fail(count.error());
    return count.value();
}

Result<void> SqliteSessionRepository::insert(const NewSession& s) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    if (s.mode != CredentialMode::Bearer || s.expiresAt <= s.createdAt) return context_.fail(ErrorCode::InvalidArgument);
    // 上限及密码快照复核由应用在同一写事务完成；仓储不签发 token 或另开事务。
    auto count = detail::write(context_, "INSERT INTO sessions(token_hash,user_id,auth_epoch,expires_at,revoked,created_at,credential_mode) SELECT ?1,id,?3,?4,0,?5,'bearer' FROM users WHERE id=?2 AND enabled=1 AND auth_epoch=?3", {detail::BlobParameter{ s.digest.bytes().data(), s.digest.bytes().size() }, s.userId.toString(), s.authEpoch.value(), s.expiresAt.time_since_epoch().count(), s.createdAt.time_since_epoch().count() });
    if (!count) return count.error();
    if (count.value() == 0) return context_.fail(ErrorCode::PreconditionFailed);
    return detail::one(context_, count);
}

Result<bool> SqliteSessionRepository::revoke(const SessionCredential& credential, UserId user, UtcTime) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    if (credential.mode != CredentialMode::Bearer) return context_.fail(ErrorCode::InvalidArgument);
    auto count = detail::write(context_, "UPDATE sessions SET revoked=1 WHERE token_hash=?1 AND user_id=?2 AND credential_mode='bearer' AND revoked=0", {detail::BlobParameter{ credential.digest.bytes().data(), credential.digest.bytes().size() }, user.toString() });
    if (!count) return count.error();
    if (count.value() > 1) return context_.fail(ErrorCode::IntegrityError);
    return count.value() == 1;
}

Result<SessionCount> SqliteSessionRepository::revokeAll(UserId user, UtcTime) {
    auto count = detail::write(context_, "UPDATE sessions SET revoked=1 WHERE user_id=?1 AND revoked=0", {user.toString() });
    if (!count) return count.error();
    auto value = SessionCount::from(count.value());
    if (!value) return context_.fail(value.error());
    return value.value();
}

Result<SessionCount> SqliteSessionRepository::eraseInactive(UtcTime now, PageSize size) {
    auto count = detail::write(context_, "DELETE FROM sessions WHERE token_hash IN (SELECT s.token_hash FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.revoked=1 OR s.expires_at<=?1 OR s.auth_epoch<>u.auth_epoch ORDER BY s.expires_at,s.token_hash LIMIT ?2)", {now.time_since_epoch().count(), size.value() });
    if (!count) return count.error();
    auto value = SessionCount::from(count.value());
    if (!value) return context_.fail(value.error());
    return value.value();
}
}  // namespace anof::infrastructure::sqlite
