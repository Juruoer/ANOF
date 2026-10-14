#include <anof/infrastructure/persistence/sqlite/SqliteUserRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

namespace anof::infrastructure::sqlite {
namespace {
constexpr std::string_view fields = "id,login,display_name,role,enabled,auth_epoch,revision,created_at,updated_at";
Result<UserProfile> profile(SqliteStatement& s) {
    auto id = readId<UserId>(s, 0);
    auto login = s.boundedTextView(1, 32);
    auto name = s.boundedTextView(2, 128);
    auto role = readUserRole(s, 3);
    auto enabled = readBoolean(s, 4);
    auto epoch = readQuantity<AuthEpoch>(s, 5);
    auto revision = readQuantity<Revision>(s, 6);
    auto created = readTime(s, 7);
    auto updated = readTime(s, 8);
    if (!id || !login || !name || !role || !enabled || !epoch || !revision || !created || !updated) return ErrorCode::IntegrityError;
    auto parsedLogin = LoginName::parse(login.value());
    auto parsedName = DisplayName::parse(name.value());
    if (!parsedLogin || parsedLogin.value().value() != login.value() || !parsedName ||
    (role.value() == UserRole::Administrator && !enabled.value())) return ErrorCode::IntegrityError;
    return UserProfile{ id.value(), std::move(parsedLogin).value(), std::move(parsedName).value(), role.value(), enabled.value(), epoch.value(), revision.value(), created.value(), updated.value() };
}
bool validHash(const EncodedPasswordHash& hash, PasswordHashLimits limits) {
    if (limits.maxMemoryKiB < 8 || limits.maxMemoryKiB > 262144 || !limits.maxIterations || limits.maxIterations > 10 || limits.maxParallelism != 1) return false;
    auto checked = EncodedPasswordHash::parse(hash.view(), limits);
    const auto text = hash.view();
    const auto saltStart = text.find('$', 16);
    const auto hashStart = saltStart == text.npos ? text.npos : text.find('$', saltStart + 1);
    return checked && saltStart != text.npos && hashStart != text.npos &&
           hashStart - saltStart - 1 == 22 && text.size() - hashStart - 1 == 43;
}
}  // namespace

Result<std::optional<UserProfile>> SqliteUserRepository::find(UserId id) {
    auto q = detail::query(context_, "SELECT "+std::string(fields)+" FROM users WHERE id=?1", {id.toString() });
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return std::optional<UserProfile>{};
    auto value = profile(*q.value());
    if (!value) return context_.fail(value.error());
    return std::optional<UserProfile>{std::move(value).value() };
}

Result<Page<UserProfile, UserListPosition>> SqliteUserRepository::list(PageSize size, std::optional<UserListPosition> after) {
    auto q = after
        ? detail::query(context_, "SELECT " + std::string(fields) + " FROM users WHERE id>?1 ORDER BY id LIMIT ?2", { after->id.toString(), size.value() + 1 })
        : detail::query(context_, "SELECT " + std::string(fields) + " FROM users ORDER BY id LIMIT ?1", { size.value() + 1 });
    if (!q) return q.error();
    Page<UserProfile, UserListPosition> page;
    for (;;) {
        auto row = q.value()->step();
        if (!row) return context_.fail(row.error());
        if (!row.value()) return page;
        auto item = profile(*q.value());
        if (!item) return context_.fail(item.error());
        if (page.items.size() == static_cast<std::size_t>(size.value())) {
            page.next = UserListPosition{ page.items.back().id };
            return page;
        }
        page.items.push_back(std::move(item).value());
    }
}

Result<std::optional<PasswordSnapshot>> SqliteUserRepository::password(std::string_view field, std::string_view key) {
    if (limits_.maxMemoryKiB < 8 || limits_.maxMemoryKiB > 262144 || !limits_.maxIterations || limits_.maxIterations > 10 || limits_.maxParallelism != 1) return context_.fail(ErrorCode::InvalidArgument);
    // field 仅由两个内部入口选择，外部输入始终绑定；秘密不经过普通 string 副本。
    auto q = detail::query(context_, "SELECT id,password_hash,password_version,auth_epoch,revision,enabled FROM users WHERE "+std::string(field)+"=?1", {key });
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return std::optional<PasswordSnapshot>{};
    auto& s = *q.value();
    auto id = readId<UserId>(s, 0);
    auto text = s.boundedTextView(1, 512);
    auto version = readQuantity<PasswordVersion>(s, 2);
    auto epoch = readQuantity<AuthEpoch>(s, 3);
    auto revision = readQuantity<Revision>(s, 4);
    auto enabled = readBoolean(s, 5);
    if (!id || !text || !version || !epoch || !revision || !enabled) return context_.fail(ErrorCode::IntegrityError);
    auto hash = EncodedPasswordHash::parse(text.value(), limits_);
    if (!hash || !validHash(hash.value(), limits_)) return context_.fail(ErrorCode::IntegrityError);
    return std::optional<PasswordSnapshot>{PasswordSnapshot{ id.value(), std::move(hash).value(), version.value(), epoch.value(), revision.value(), enabled.value() } };
}

Result<std::optional<PasswordSnapshot>> SqliteUserRepository::findPassword(LoginName login) {
    return password("login", login.value());
}

Result<std::optional<PasswordSnapshot>> SqliteUserRepository::findPasswordById(UserId id) {
    return password("id", id.toString());
}

Result<UserCount> SqliteUserRepository::countUsers() {
    auto q = detail::query(context_, "SELECT count(*) FROM users");
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return context_.fail(ErrorCode::IntegrityError);
    auto value = readQuantity<UserCount>(*q.value(), 0);
    if (!value) return context_.fail(value.error());
    return value.value();
}

Result<void> SqliteUserRepository::insert(const NewUser& user) {
    auto ready = context_.requireOpen();
    if (!ready) return ready.error();
    const auto& u = user.profile;
    if (u.revision.value() != 1 || u.authEpoch.value() != 0 || user.passwordVersion.value() != 1 || !u.enabled || u.createdAt != u.updatedAt || !validHash(user.encodedHash, limits_) ||
    (u.role != UserRole::Member && u.role != UserRole::Administrator)) return context_.fail(ErrorCode::InvalidArgument);
    // ID 碰撞不能误报为登录名冲突。
    auto existing = find(u.id);
    if (!existing) return existing.error();
    if (existing.value()) return context_.fail(ErrorCode::IntegrityError);
    auto name = detail::query(context_, "SELECT 1 FROM users WHERE login=?1", {u.login.value() });
    if (!name) return name.error();
    auto row = name.value()->step();
    if (!row) return context_.fail(row.error());
    if (row.value()) return context_.fail(ErrorCode::LoginConflict);
    name.value().reset();
    return detail::one(context_, detail::write(context_,
    "INSERT INTO users(id,login,display_name,role,enabled,auth_epoch,password_hash,password_version,revision,created_at,updated_at) VALUES(?1,?2,?3,?4,1,0,?5,1,1,?6,?6)",
    {
        u.id.toString(), u.login.value(), u.displayName.value(), std::int64_t(u.role == UserRole::Administrator ? 1:0), user.encodedHash.view(), u.createdAt.time_since_epoch().count() }));
}

Result<UserProfile> SqliteUserRepository::setDisplayName(UserId id, Revision expected, DisplayName name, UtcTime now) {
    auto value = find(id);
    if (!value) return value.error();
    if (!value.value()) return context_.fail(ErrorCode::NotFound);
    auto user = std::move(*value.value());
    if (user.revision != expected) return context_.fail(ErrorCode::PreconditionFailed);
    if (user.displayName.value() == name.value()) return user;
    auto next = user.revision.add(1);
    if (!next) return context_.fail(next.error());
    auto changed = detail::one(context_, detail::write(context_, "UPDATE users SET display_name=?1,revision=?2,updated_at=?3 WHERE id=?4 AND revision=?5", {name.value(), next.value().value(), now.time_since_epoch().count(), id.toString(), expected.value() }));
    if (!changed) return changed.error();
    user.displayName = std::move(name);
    user.revision = next.value();
    user.updatedAt = now;
    return user;
}

Result<UserProfile> SqliteUserRepository::setEnabled(UserId id, Revision expected, bool enabled, UtcTime now) {
    auto value = find(id);
    if (!value) return value.error();
    if (!value.value()) return context_.fail(ErrorCode::NotFound);
    auto user = std::move(*value.value());
    if (user.revision != expected) return context_.fail(ErrorCode::PreconditionFailed);
    if (user.role == UserRole::Administrator && !enabled) return context_.fail(ErrorCode::Forbidden);
    if (user.enabled == enabled) return user;
    auto next = user.revision.add(1);
    auto epoch = user.authEpoch.add(enabled ? 0:1);
    if (!next || !epoch) return context_.fail(ErrorCode::IntegrityError);
    auto changed = detail::one(context_, detail::write(context_, "UPDATE users SET enabled=?1,auth_epoch=?2,revision=?3,updated_at=?4 WHERE id=?5 AND revision=?6", {std::int64_t(enabled ? 1:0), epoch.value().value(), next.value().value(), now.time_since_epoch().count(), id.toString(), expected.value() }));
    if (!changed) return changed.error();
    user.enabled = enabled;
    user.authEpoch = epoch.value();
    user.revision = next.value();
    user.updatedAt = now;
    return user;
}

Result<UserProfile> SqliteUserRepository::replacePassword(UserId id, const PasswordExpectation& expected, const EncodedPasswordHash& hash, UtcTime now) {
    auto value = findPasswordById(id);
    if (!value) return value.error();
    if (!value.value()) return context_.fail(ErrorCode::NotFound);
    const auto& snapshot = *value.value();
    if (!snapshot.enabled || snapshot.passwordVersion != expected.passwordVersion || snapshot.authEpoch != expected.authEpoch || snapshot.userRevision != expected.userRevision) return context_.fail(ErrorCode::PreconditionFailed);
    if (!validHash(hash, limits_)) return context_.fail(ErrorCode::InvalidArgument);
    auto pv = snapshot.passwordVersion.add(1);
    auto epoch = snapshot.authEpoch.add(1);
    auto rev = snapshot.userRevision.add(1);
    if (!pv || !epoch || !rev) return context_.fail(ErrorCode::IntegrityError);
    auto changed = detail::one(context_, detail::write(context_, "UPDATE users SET password_hash=?1,password_version=?2,auth_epoch=?3,revision=?4,updated_at=?5 WHERE id=?6 AND password_version=?7 AND auth_epoch=?8 AND revision=?9 AND enabled=1",
    {
        hash.view(), pv.value().value(), epoch.value().value(), rev.value().value(), now.time_since_epoch().count(), id.toString(), expected.passwordVersion.value(), expected.authEpoch.value(), expected.userRevision.value() }));
    if (!changed) return changed.error();
    auto user = find(id);
    if (!user) return user.error();
    if (!user.value()) return context_.fail(ErrorCode::IntegrityError);
    return std::move(*user.value());
}
}  // namespace anof::infrastructure::sqlite
