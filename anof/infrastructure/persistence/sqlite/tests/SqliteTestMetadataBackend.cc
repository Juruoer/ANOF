#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestStore.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestMetadataBackend.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace anof::infrastructure::sqlite::testing {
namespace {

// 绑定失败时绝不 step；所有值仍通过参数绑定进入测试库。
Result<void> finish(SqliteStatement& statement, std::initializer_list<Result<void>> bindings) {
    for (const auto& binding : bindings) if (!binding) return binding.error();
    auto row = statement.step();
    if (!row) return row.error();
    if (row.value()) return ErrorCode::IntegrityError;
    return {};
}

Result<void> seedDatabase(SqliteConnection& connection, const anof::testing::MetadataSeed& seed) {
    // 节点种子顺序不必按拓扑排序；所有外键仍须在提交时成立。
    auto deferred = connection.execute("PRAGMA defer_foreign_keys=ON");
    if (!deferred) return deferred.error();
    std::size_t userIndex = 0;
    for (const auto& user : seed.users) {
        auto p = SqliteStatement::prepare(connection, "INSERT INTO users VALUES (?1, ?2, ?3, ?4, ?5, 'Fixture user', '"
            "$argon2id$v=19$m=8192,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA', 1, 1, 0, 0)");
        if (!p) return p.error();
        auto& s = *p.value();
        if (user.principal.role != UserRole::Member && user.principal.role != UserRole::Administrator) return ErrorCode::IntegrityError;
        auto result = finish(s, { s.bindText(1, user.principal.userId.toString()),
            s.bindInt64(2, user.principal.role == UserRole::Administrator ? 1 : 0), s.bindInt64(3, user.enabled ? 1 : 0),
            s.bindInt64(4, user.principal.authEpoch.value()), s.bindText(5, "fixture_" + std::to_string(++userIndex)) });
        if (!result) return result.error();
    }
    for (const auto& session : seed.sessions) {
        auto p = SqliteStatement::prepare(connection, "INSERT INTO sessions VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'bearer')");
        if (!p) return p.error();
        auto& s = *p.value();
        const auto expiry = session.expiresAt.time_since_epoch().count();
        if (expiry == std::numeric_limits<std::int64_t>::min()) return ErrorCode::IntegrityError;
        auto result = finish(s, { s.bindBlob(1, session.digest.bytes().data(), session.digest.bytes().size()),
            s.bindText(2, session.userId.toString()), s.bindInt64(3, session.authEpoch.value()),
            s.bindInt64(4, session.expiresAt.time_since_epoch().count()), s.bindInt64(5, session.revoked ? 1 : 0), s.bindInt64(6, expiry - 1) });
        if (!result) return result.error();
    }
    for (const auto& space : seed.spaces) {
        auto p = SqliteStatement::prepare(connection, "INSERT INTO spaces VALUES (?1, ?2, ?3, ?4)");
        if (!p) return p.error();
        auto& s = *p.value();
        if (space.kind != SpaceKind::Private && space.kind != SpaceKind::Public) return ErrorCode::IntegrityError;
        auto result = finish(s, { s.bindText(1, space.id.toString()), s.bindInt64(2, space.kind == SpaceKind::Public ? 1 : 0),
            space.ownerId ? s.bindText(3, space.ownerId->toString()) : s.bindNull(3), s.bindText(4, space.rootNodeId.toString()) });
        if (!result) return result.error();
    }
    for (const auto& blob : seed.blobs) {
        auto p = SqliteStatement::prepare(connection, "INSERT INTO blobs VALUES (?1, ?2, ?3)");
        if (!p) return p.error();
        auto& s = *p.value();
        auto result = finish(s, { s.bindText(1, blob.id.toString()), s.bindInt64(2, blob.size.value()),
            s.bindBlob(3, blob.sha256.data(), blob.sha256.size()) });
        if (!result) return result.error();
    }
    for (const auto& node : seed.nodes) {
        auto p = SqliteStatement::prepare(connection, "INSERT INTO nodes VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
        if (!p) return p.error();
        auto& s = *p.value();
        const auto& n = node.metadata;
        auto result = finish(s, { s.bindText(1, n.id.toString()), s.bindText(2, n.spaceId.toString()),
            n.parentId ? s.bindText(3, n.parentId->toString()) : s.bindNull(3), s.bindText(4, n.name),
            s.bindInt64(5, node.blobId ? 1 : 0), s.bindText(6, n.createdBy.toString()),
            node.blobId ? s.bindText(7, node.blobId->toString()) : s.bindNull(7), s.bindInt64(8, n.revision.value()),
            s.bindInt64(9, node.trashed ? 1 : 0), s.bindInt64(10, n.createdAt.time_since_epoch().count()),
            s.bindInt64(11, n.updatedAt.time_since_epoch().count()) });
        if (!result) return result.error();
    }
    for (const auto& quota : seed.quotas) {
        const auto* contribution = std::get_if<PublicContributionQuota>(&quota.scope);
        const auto space = contribution ? contribution->spaceId : std::get<SpaceQuota>(quota.scope).spaceId;
        auto p = SqliteStatement::prepare(connection, contribution ?
            "INSERT INTO public_contribution_quotas(space_id, limit_bytes, limit_nodes, used_bytes, used_nodes, reserved_bytes, reserved_nodes, user_id, revision) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 1)" :
            "INSERT INTO space_quotas VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 1)");
        if (!p) return p.error();
        auto& s = *p.value();
        if (contribution) {
            auto bound = s.bindText(8, contribution->userId.toString());
            if (!bound) return bound.error();
        }
        const auto& q = quota.state;
        auto result = finish(s, { s.bindText(1, space.toString()), s.bindInt64(2, q.limit.bytes.value()), s.bindInt64(3, q.limit.nodes.value()),
            s.bindInt64(4, q.used.bytes.value()), s.bindInt64(5, q.used.nodes.value()),
            s.bindInt64(6, q.reserved.bytes.value()), s.bindInt64(7, q.reserved.nodes.value()) });
        if (!result) return result.error();
    }
    return {};
}

}  // namespace

SqliteTestMetadataBackend::SqliteTestMetadataBackend() {
    char pattern[] = "/tmp/anof-sqlite-contract-XXXXXX";
    const char* path = mkdtemp(pattern);
    if (!path) throw std::runtime_error("Cannot create SQLite test directory");
    directory_ = path;
}

SqliteTestMetadataBackend::~SqliteTestMetadataBackend() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
}

std::filesystem::path SqliteTestMetadataBackend::databasePath() const { return directory_ / "database" / "metadata.sqlite3"; }

Result<void> SqliteTestMetadataBackend::reset(const anof::testing::MetadataSeed& seed) {
    std::error_code error;
    // 只清理本 fixture 拥有的数据库及 WAL 文件；调用方须先关闭所有连接。
    std::filesystem::remove_all(directory_ / "database", error);
    if (error) return ErrorCode::InternalError;
    std::filesystem::create_directory(directory_ / "database", error);
    if (error) return ErrorCode::InternalError;
    auto created = createSqliteDatabase(databasePath(), { 0 });
    if (!created) return created.error();
    auto connection = openSqliteDatabase(databasePath(), { 0 });
    if (!connection) return connection.error();
    auto begun = connection.value()->beginWrite();
    if (!begun) return begun.error();
    auto seeded = seedDatabase(*connection.value(), seed);
    if (!seeded) return seeded.error();
    // 失败和异常退出时连接析构回滚；提交失败也不会残留半份 seed。
    return connection.value()->commit();
}

Result<std::unique_ptr<MetadataStore>> SqliteTestMetadataBackend::connect() { return testing::openTestMetadataStore(databasePath(), { 0 }); }

Result<std::optional<anof::testing::NodeSeed>> SqliteTestMetadataBackend::readNode(NodeId nodeId) {
    auto connection = SqliteConnection::open(databasePath(), { 0 }, SqliteOpenMode::Inspect);
    if (!connection) return connection.error();
    auto p = SqliteStatement::prepare(*connection.value(),
        "SELECT id, space_id, parent_id, name, created_by, revision, created_at, updated_at, blob_id, trashed, kind FROM nodes WHERE id=?1");
    if (!p) return p.error();
    auto& s = *p.value();
    auto bound = s.bindText(1, nodeId.toString());
    if (!bound) return bound.error();
    auto row = s.step();
    if (!row) return row.error();
    if (!row.value()) return std::optional<anof::testing::NodeSeed>{};
    auto storedId = readId<NodeId>(s, 0);
    auto space = readId<SpaceId>(s, 1);
    auto parentNull = s.isNull(2);
    auto name = s.text(3);
    auto creator = readId<UserId>(s, 4);
    auto revision = readQuantity<Revision>(s, 5);
    auto created = readTime(s, 6);
    auto updated = readTime(s, 7);
    auto blobNull = s.isNull(8);
    auto trashed = readBoolean(s, 9);
    auto kind = s.integer(10);
    if (!storedId || !space || !parentNull || !name || !creator || !revision || !created || !updated || !blobNull || !trashed || !kind) return ErrorCode::IntegrityError;
    if (storedId.value() != nodeId || (kind.value() != 0 && kind.value() != 1) || (kind.value() == 1) == blobNull.value()) return ErrorCode::IntegrityError;
    std::optional<NodeId> parent;
    if (!parentNull.value()) {
        auto value = readId<NodeId>(s, 2);
        if (!value) return value.error();
        parent = value.value();
    }
    std::optional<BlobId> blob;
    if (!blobNull.value()) {
        auto value = readId<BlobId>(s, 8);
        if (!value) return value.error();
        blob = value.value();
    }
    return std::optional<anof::testing::NodeSeed>{ { { storedId.value(), space.value(), parent, name.value(), creator.value(), revision.value(), created.value(), updated.value() }, blob, trashed.value() } };
}

Result<QuotaState> SqliteTestMetadataBackend::readQuota(const QuotaScope& scope) {
    auto connection = SqliteConnection::open(databasePath(), { 0 }, SqliteOpenMode::Inspect);
    if (!connection) return connection.error();
    const auto* contribution = std::get_if<PublicContributionQuota>(&scope);
    const auto space = contribution ? contribution->spaceId : std::get<SpaceQuota>(scope).spaceId;
    auto p = SqliteStatement::prepare(*connection.value(), contribution ?
        "SELECT limit_bytes, limit_nodes, used_bytes, used_nodes, reserved_bytes, reserved_nodes FROM public_contribution_quotas WHERE space_id=?1 AND user_id=?2" :
        "SELECT limit_bytes, limit_nodes, used_bytes, used_nodes, reserved_bytes, reserved_nodes FROM space_quotas WHERE space_id=?1");
    if (!p) return p.error();
    auto& s = *p.value();
    auto bound = s.bindText(1, space.toString());
    if (!bound) return bound.error();
    if (contribution) {
        bound = s.bindText(2, contribution->userId.toString());
        if (!bound) return bound.error();
    }
    auto row = s.step();
    if (!row) return row.error();
    if (!row.value()) return ErrorCode::IntegrityError;
    auto lb = readQuantity<ByteCount>(s, 0);
    auto ln = readQuantity<NodeCount>(s, 1);
    auto ub = readQuantity<ByteCount>(s, 2);
    auto un = readQuantity<NodeCount>(s, 3);
    auto rb = readQuantity<ByteCount>(s, 4);
    auto rn = readQuantity<NodeCount>(s, 5);
    if (!lb || !ln || !ub || !un || !rb || !rn) return ErrorCode::IntegrityError;
    return QuotaState{ { lb.value(), ln.value() }, { ub.value(), un.value() }, { rb.value(), rn.value() } };
}

Result<std::vector<DirectoryCreatedEvent>> SqliteTestMetadataBackend::readAudit() {
    auto connection = SqliteConnection::open(databasePath(), { 0 }, SqliteOpenMode::Inspect);
    if (!connection) return connection.error();
    auto p = SqliteStatement::prepare(*connection.value(),
        "SELECT actor_id, space_id, node_id, parent_id, request_id, occurred_at, action FROM audit_events ORDER BY sequence");
    if (!p) return p.error();
    auto& s = *p.value();
    std::vector<DirectoryCreatedEvent> events;
    for (;;) {
        auto row = s.step();
        if (!row) return row.error();
        if (!row.value()) return events;
        auto actor = readId<UserId>(s, 0);
        auto space = readId<SpaceId>(s, 1);
        auto node = readId<NodeId>(s, 2);
        auto parent = readId<NodeId>(s, 3);
        auto request = readId<RequestId>(s, 4);
        auto occurred = readTime(s, 5);
        auto action = s.text(6);
        if (!actor || !space || !node || !parent || !request || !occurred || !action || action.value() != "directory_created") return ErrorCode::IntegrityError;
        events.push_back({ actor.value(), space.value(), node.value(), parent.value(), request.value(), occurred.value() });
    }
}

}  // namespace anof::infrastructure::sqlite::testing

namespace anof::testing {

std::unique_ptr<TestMetadataBackend> makeTestMetadataBackend() {
    return std::make_unique<infrastructure::sqlite::testing::SqliteTestMetadataBackend>();
}

}  // namespace anof::testing
