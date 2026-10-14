#include <anof/infrastructure/persistence/sqlite/SqliteMetadataReadSnapshot.h>
#include <anof/infrastructure/persistence/sqlite/SqliteAuthorizationReader.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUserRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSessionRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInviteRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSpaceRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteNodeRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteQuotaRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSettingsRepository.h>

namespace anof::infrastructure::sqlite {
namespace {
// 组合复用查询；对外动态类型只实现 Reader，不能向下转型为 Repository。
class UserView final : public UserReader {
public:
    UserView(SqliteTransactionContext& context, PasswordHashLimits limits) : implementation_(context, limits) {}
    Result<std::optional<UserProfile>> find(UserId id) override { return implementation_.find(id); }
    Result<Page<UserProfile, UserListPosition>> list(PageSize size, std::optional<UserListPosition> after) override { return implementation_.list(size, std::move(after)); }
    Result<std::optional<PasswordSnapshot>> findPassword(LoginName login) override { return implementation_.findPassword(std::move(login)); }
    Result<std::optional<PasswordSnapshot>> findPasswordById(UserId id) override { return implementation_.findPasswordById(id); }
    Result<UserCount> countUsers() override { return implementation_.countUsers(); }
private:
    SqliteUserRepository implementation_;
};

class SessionView final : public SessionReader {
public:
    SessionView(SqliteTransactionContext& context) : implementation_(context) {}
    Result<SessionCount> countActive(UserId id, UtcTime now) override { return implementation_.countActive(id, now); }
private:
    SqliteSessionRepository implementation_;
};

class InviteView final : public InviteReader {
public:
    InviteView(SqliteTransactionContext& context) : implementation_(context) {}
    Result<std::optional<InviteInfo>> find(InviteId id) override { return implementation_.find(id); }
    Result<Page<InviteInfo, InviteListPosition>> list(PageSize size, std::optional<InviteListPosition> after) override { return implementation_.list(size, std::move(after)); }
private:
    SqliteInviteRepository implementation_;
};

class SpaceView final : public SpaceReader {
public:
    SpaceView(SqliteTransactionContext& context) : implementation_(context) {}
    Result<std::optional<Space>> find(SpaceId id) override { return implementation_.find(id); }
    Result<std::optional<Space>> findPrivate(UserId id) override { return implementation_.findPrivate(id); }
    Result<std::vector<Space>> listAccessible(UserId id) override { return implementation_.listAccessible(id); }
private:
    SqliteSpaceRepository implementation_;
};

class NodeView final : public NodeReader {
public:
    NodeView(SqliteTransactionContext& context) : implementation_(context) {}
    Result<DirectoryPath> findActiveDirectoryPath(SpaceId space, NodeId parent) override { return implementation_.findActiveDirectoryPath(space, parent); }
    Result<std::optional<NodeDetails>> find(NodeId id) override { return implementation_.find(id); }
    Result<Page<NodeDetails, ChildPosition>> listChildren(SpaceId space, NodeId parent, PageSize size, std::optional<ChildPosition> after) override { return implementation_.listChildren(space, parent, size, std::move(after)); }
private:
    SqliteNodeRepository implementation_;
};

class QuotaView final : public QuotaReader {
public:
    QuotaView(SqliteTransactionContext& context) : implementation_(context) {}
    Result<QuotaSnapshot> get(const QuotaScope& scope) override { return implementation_.get(scope); }
private:
    SqliteQuotaRepository implementation_;
};

class SettingsView final : public SettingsReader {
public:
    SettingsView(SqliteTransactionContext& context) : implementation_(context) {}
    Result<std::optional<InstanceSettings>> get() override { return implementation_.get(); }
private:
    SqliteSettingsRepository implementation_;
};

class Snapshot final : public MetadataReadSnapshot {
public:
    Snapshot(SqliteConnection& connection, bool& active, PasswordHashLimits limits)
        : context_(connection, active), authorization_(context_), users_(context_, limits), sessions_(context_), invites_(context_), spaces_(context_), nodes_(context_), quotas_(context_), settings_(context_) {}
    Result<void> begin() { return context_.beginRead(); }
    AuthorizationReader& authorization() override { return authorization_; }
    UserReader& users() override { return users_; }
    SessionReader& sessions() override { return sessions_; }
    InviteReader& invites() override { return invites_; }
    SpaceReader& spaces() override { return spaces_; }
    NodeReader& nodes() override { return nodes_; }
    QuotaReader& quotas() override { return quotas_; }
    SettingsReader& settings() override { return settings_; }
private:
    SqliteTransactionContext context_;
    SqliteAuthorizationReader authorization_;
    UserView users_;
    SessionView sessions_;
    InviteView invites_;
    SpaceView spaces_;
    NodeView nodes_;
    QuotaView quotas_;
    SettingsView settings_;
};
}  // namespace

Result<std::unique_ptr<MetadataReadSnapshot>> makeSqliteReadSnapshot(SqliteConnection& connection, bool& active, PasswordHashLimits limits) {
    auto snapshot = std::make_unique<Snapshot>(connection, active, limits);
    auto begun = snapshot->begin();
    if (!begun) return begun.error();
    return std::unique_ptr<MetadataReadSnapshot>(std::move(snapshot));
}
}  // namespace anof::infrastructure::sqlite
