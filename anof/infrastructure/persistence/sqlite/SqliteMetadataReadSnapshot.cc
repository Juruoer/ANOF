#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteAuthorizationReader.h>
#include <anof/infrastructure/persistence/sqlite/SqliteBlobRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInviteRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataReadSnapshot.h>
#include <anof/infrastructure/persistence/sqlite/SqliteNodeAccountingRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteNodeRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteQuotaRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSessionRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSettingsRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSpaceRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStorageRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTrashRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTreeRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUploadRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUserRepository.h>
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

class UploadView final : public UploadReader {
    SqliteUploadRepository impl_;

public:
    explicit UploadView(SqliteTransactionContext& c) : impl_(c) {}
    Result<std::optional<Upload>> find(UploadId id) override { return impl_.find(id); }
    Result<std::optional<Upload>> findByClientId(UserId u, OperationId id) override { return impl_.findByClientId(u, id); }
    Result<UploadCounts> counts(UserId u) override { return impl_.counts(u); }
    Result<Page<Upload, UploadPosition>> list(UserId u, PageSize s, std::optional<UploadPosition> p) override { return impl_.list(u, s, p); }
    Result<Page<Upload, UploadPosition>> scan(UploadScan scan, UtcTime n, PageSize s, std::optional<UploadPosition> p) override { return impl_.scan(scan, n, s, p); }
    Result<ByteCount> unwrittenCommitment() override { return impl_.unwrittenCommitment(); }
};
class BlobView final : public BlobReader {
    SqliteBlobRepository impl_;

public:
    explicit BlobView(SqliteTransactionContext& c) : impl_(c) {}
    Result<std::optional<Blob>> find(BlobId id) override { return impl_.find(id); }
    Result<std::optional<Blob>> findByNode(NodeId id) override { return impl_.findByNode(id); }
    Result<std::optional<BlobId>> candidateForUpload(UploadId id) override { return impl_.candidateForUpload(id); }
    Result<bool> hasNodeReference(BlobId id) override { return impl_.hasNodeReference(id); }
    Result<bool> hasUploadReference(BlobId id) override { return impl_.hasUploadReference(id); }
    Result<Page<Blob, BlobId>> list(PageSize s, std::optional<BlobId> p) override { return impl_.list(s, p); }
};
class StorageView final : public StorageReader {
    SqliteStorageRepository impl_;

public:
    explicit StorageView(SqliteTransactionContext& c) : impl_(c) {}
    Result<StorageSnapshot> get() override { return impl_.get(); }
};
class TreeView final : public TreeReader {
    SqliteTreeRepository impl_;

public:
    explicit TreeView(SqliteTransactionContext& c) : impl_(c) {}
    Result<TreeSnapshot> activeSubtree(NodeId id, QueryBudget b) override { return impl_.activeSubtree(id, b); }
    Result<TreeSnapshot> trashSubtree(TrashId id, QueryBudget b) override { return impl_.trashSubtree(id, b); }
    Result<Page<NodeDetails, ChildPosition>> search(const Principal& p, const SearchQuery& q) override { return impl_.search(p, q); }
};
class TrashView final : public TrashReader {
    SqliteTrashRepository impl_;

public:
    explicit TrashView(SqliteTransactionContext& c) : impl_(c) {}
    Result<std::optional<TrashEntry>> find(TrashId id) override { return impl_.find(id); }
    Result<Page<TrashEntry, TrashPosition>> list(const TrashQuery& q) override { return impl_.list(q); }
    Result<Page<TrashEntry, TrashExpiryPosition>> expired(UtcTime n, PageSize s, std::optional<TrashExpiryPosition> p) override { return impl_.expired(n, s, p); }
};
class NodeAccountingView final : public NodeAccountingReader {
    SqliteNodeAccountingRepository impl_;

public:
    explicit NodeAccountingView(SqliteTransactionContext& c) : impl_(c) {}
    Result<NodeAccounting> get() override { return impl_.get(); }
};
class BlobLifecycleView final : public BlobLifecycleReader {
    SqliteBlobRepository impl_;

public:
    explicit BlobLifecycleView(SqliteTransactionContext& c) : impl_(c) {}
    Result<std::optional<BlobRecord>> record(BlobId id) override { return impl_.record(id); }
    Result<Page<BlobRecord, BlobGcPosition>> gcCandidates(UtcTime n, PageSize s, std::optional<BlobGcPosition> p) override { return impl_.gcCandidates(n, s, p); }
    Result<Page<BlobRecord, BlobId>> deleting(PageSize s, std::optional<BlobId> p) override { return impl_.deleting(s, p); }
    Result<GcStatistics> gcStatistics() override { return impl_.gcStatistics(); }
};
class Snapshot final : public DriveReadSnapshot {
public:
    Snapshot(SqliteConnection& connection, bool& active, PasswordHashLimits limits)
        : context_(connection, active), authorization_(context_), users_(context_, limits), sessions_(context_), invites_(context_), spaces_(context_), nodes_(context_), quotas_(context_), settings_(context_), uploads_(context_), blobs_(context_), storage_(context_), tree_(context_), trash_(context_), lifecycle_(context_), nodeAccounting_(context_) {}
    Result<void> begin() { return context_.beginRead(); }
    AuthorizationReader& authorization() override { return authorization_; }
    UserReader& users() override { return users_; }
    SessionReader& sessions() override { return sessions_; }
    InviteReader& invites() override { return invites_; }
    SpaceReader& spaces() override { return spaces_; }
    NodeReader& nodes() override { return nodes_; }
    QuotaReader& quotas() override { return quotas_; }
    SettingsReader& settings() override { return settings_; }
    UploadReader& uploads() override { return uploads_; }
    BlobReader& blobs() override { return blobs_; }
    StorageReader& storage() override { return storage_; }

    TreeReader& tree() override { return tree_; }
    TrashReader& trash() override { return trash_; }
    BlobLifecycleReader& blobLifecycle() override { return lifecycle_; }
    NodeAccountingReader& nodeAccounting() override { return nodeAccounting_; }

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
    UploadView uploads_;
    BlobView blobs_;
    StorageView storage_;
    TreeView tree_;
    TrashView trash_;
    BlobLifecycleView lifecycle_;
    NodeAccountingView nodeAccounting_;
};
}  // namespace

Result<std::unique_ptr<DriveReadSnapshot>> makeSqliteDriveReadSnapshot(SqliteConnection& connection, bool& active, PasswordHashLimits limits) {
    auto snapshot = std::make_unique<Snapshot>(connection, active, limits);
    auto begun = snapshot->begin();
    if (!begun) return begun.error();
    return std::unique_ptr<DriveReadSnapshot>(std::move(snapshot));
}
Result<std::unique_ptr<TransferReadSnapshot>> makeSqliteTransferReadSnapshot(SqliteConnection& c, bool& active, PasswordHashLimits limits) {
    auto s = makeSqliteDriveReadSnapshot(c, active, limits);
    if (!s) return s.error();
    return std::unique_ptr<TransferReadSnapshot>(std::move(s).value());
}
Result<std::unique_ptr<MetadataReadSnapshot>> makeSqliteReadSnapshot(SqliteConnection& c, bool& active, PasswordHashLimits limits) {
    auto snapshot = makeSqliteTransferReadSnapshot(c, active, limits);
    if (!snapshot) return snapshot.error();
    return std::unique_ptr<MetadataReadSnapshot>(std::move(snapshot).value());
}
}  // namespace anof::infrastructure::sqlite
