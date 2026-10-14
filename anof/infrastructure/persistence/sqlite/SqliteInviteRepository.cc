#include <anof/infrastructure/persistence/sqlite/SqliteInviteRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <limits>

namespace anof::infrastructure::sqlite {
namespace {
constexpr const char* fields = "id,created_by,created_at,expires_at,max_uses,used_count,revoked_at";
Result<InviteInfo> decode(SqliteStatement& s) {
    auto id = readId<InviteId>(s, 0);
    auto creator = readId<UserId>(s, 1);
    auto created = readTime(s, 2);
    auto expires = readTime(s, 3);
    auto maximum = readQuantity<InviteUseLimit>(s, 4);
    auto used = readQuantity<InviteUseCount>(s, 5);
    auto null = s.isNull(6);
    if (!id || !creator || !created || !expires || !maximum || !used || !null || expires.value() <= created.value() || used.value().value()>maximum.value().value()) return ErrorCode::IntegrityError;
    const auto start = created.value().time_since_epoch().count();
    if (start <= std::numeric_limits<std::int64_t>::max()-2592000000000LL && expires.value().time_since_epoch().count()>start + 2592000000000LL) return ErrorCode::IntegrityError;
    std::optional<UtcTime> revoked;
    if (!null.value()) {
        auto time = readTime(s, 6);
        if (!time) return time.error();
        revoked = time.value();
    }
    return InviteInfo{ id.value(), creator.value(), created.value(), expires.value(), maximum.value(), used.value(), revoked };
}
}  // namespace

Result<std::optional<InviteInfo>> SqliteInviteRepository::find(InviteId id) {
    auto q = detail::query(context_, std::string("SELECT ") + fields+" FROM invites WHERE id=?1", {id.toString() });
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return std::optional<InviteInfo>{};
    auto value = decode(*q.value());
    if (!value) return context_.fail(value.error());
    return std::optional<InviteInfo>{value.value() };
}

Result<Page<InviteInfo, InviteListPosition>> SqliteInviteRepository::list(PageSize size, std::optional<InviteListPosition> after) {
    auto q = after ? detail::query(context_, std::string("SELECT ") + fields+" FROM invites WHERE (created_at,id)>(?1,?2) ORDER BY created_at,id LIMIT ?3", {after->createdAt.time_since_epoch().count(), after->id.toString(), size.value() + 1 })
    : detail::query(context_, std::string("SELECT ") + fields+" FROM invites ORDER BY created_at,id LIMIT ?1", {size.value() + 1 });
    if (!q) return q.error();
    Page<InviteInfo, InviteListPosition> page;
    for (;;) {
        auto row = q.value()->step();
        if (!row) return context_.fail(row.error());
        if (!row.value()) return page;
        auto item = decode(*q.value());
        if (!item) return context_.fail(item.error());
        if (page.items.size() == static_cast<std::size_t>(size.value())) {
            const auto& last = page.items.back();
            page.next = InviteListPosition{ last.createdAt, last.id };
            return page;
        }
        page.items.push_back(item.value());
    }
}

Result<void> SqliteInviteRepository::insert(const NewInvite& invite) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    const auto& i = invite.info;
    const auto start = i.createdAt.time_since_epoch().count();
    if (i.usedCount.value() != 0 || i.revokedAt || i.expiresAt <= i.createdAt ||
    (start <= std::numeric_limits<std::int64_t>::max()-2592000000000LL && i.expiresAt.time_since_epoch().count()>start + 2592000000000LL)) return context_.fail(ErrorCode::InvalidArgument);
    return detail::one(context_, detail::write(context_, "INSERT INTO invites VALUES(?1,?2,?3,?4,?5,?6,0,NULL)", {i.id.toString(), detail::BlobParameter{ invite.digest.bytes().data(), invite.digest.bytes().size() }, i.createdBy.toString(), start, i.expiresAt.time_since_epoch().count(), i.maxUses.value() }));
}

Result<InviteInfo> SqliteInviteRepository::consume(const InviteDigest& digest, UtcTime now) {
    auto q = detail::query(context_, std::string("UPDATE invites SET used_count=used_count+1 WHERE token_hash=?1 AND revoked_at IS NULL AND expires_at>?2 AND used_count<max_uses RETURNING ") + fields, {detail::BlobParameter{ digest.bytes().data(), digest.bytes().size() }, now.time_since_epoch().count() });
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return context_.fail(ErrorCode::InviteUnavailable);
    auto value = decode(*q.value());
    if (!value) return context_.fail(value.error());
    row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (row.value()) return context_.fail(ErrorCode::IntegrityError);
    return value.value();
}

Result<bool> SqliteInviteRepository::revoke(InviteId id, UtcTime now) {
    auto value = find(id);
    if (!value) return value.error();
    if (!value.value()) return context_.fail(ErrorCode::NotFound);
    if (value.value()->revokedAt) return false;
    auto result = detail::one(context_, detail::write(context_, "UPDATE invites SET revoked_at=?1 WHERE id=?2 AND revoked_at IS NULL", {now.time_since_epoch().count(), id.toString() }));
    if (!result) return result.error();
    return true;
}
}  // namespace anof::infrastructure::sqlite
