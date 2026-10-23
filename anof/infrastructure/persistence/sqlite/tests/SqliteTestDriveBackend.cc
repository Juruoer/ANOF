#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStoreInternal.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTrashRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestDriveBackend.h>

#include <cstdlib>
#include <map>
#include <stdexcept>
namespace anof::infrastructure::sqlite {
Result<void> seedTransferUpload(SqliteTransactionContext&, const Upload&);
namespace testing {
Result<void> seedDriveMetadata(SqliteConnection&, const anof::testing::DriveSeed&);
SqliteTestDriveBackend::SqliteTestDriveBackend() {
    char pattern[] = "/tmp/anof-drive-db-XXXXXX";
    auto p = mkdtemp(pattern);
    if (!p) throw std::runtime_error("temporary directory failed");
    root_ = p;
}
SqliteTestDriveBackend::~SqliteTestDriveBackend() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
}
Result<void> SqliteTestDriveBackend::reset(const anof::testing::DriveSeed& seed) {
    if (!validateNodeAccounting(seed.nodeAccounting) || seed.nodeAccounting.used.value() != static_cast<std::int64_t>(seed.transfer.metadata.nodes.size())) return ErrorCode::IntegrityError;
    std::int64_t reservations = 0;
    for (const auto& u : seed.transfer.uploads)
        if (static_cast<int>(u.state) < 5) ++reservations;
    if (seed.nodeAccounting.reserved.value() != reservations) return ErrorCode::IntegrityError;
    if (seed.blobRecords.size() != seed.transfer.metadata.blobs.size()) return ErrorCode::IntegrityError;
    std::map<BlobId, BlobRecord> records;
    for (const auto& b : seed.blobRecords)
        if (!validateBlobRecord(b) || !records.emplace(b.content.id, b).second) return ErrorCode::IntegrityError;
    for (const auto& b : seed.transfer.metadata.blobs) {
        auto it = records.find(b.id);
        if (it == records.end() || it->second.content.size.value() != b.size.value() || it->second.content.sha256.bytes() != b.sha256) return ErrorCode::IntegrityError;
    }
    std::map<NodeId, TrashId> membership;
    for (auto p : seed.memberships)
        if (!membership.emplace(p).second) return ErrorCode::IntegrityError;
    for (const auto& pair : membership)
        if (std::none_of(seed.transfer.metadata.nodes.begin(), seed.transfer.metadata.nodes.end(), [&](const auto& n) { return n.metadata.id == pair.first; })) return ErrorCode::IntegrityError;
    ByteCount retained, reserved;
    for (const auto& b : seed.blobRecords) {
        auto next = retained.add(b.content.size.value());
        if (!next) return next.error();
        retained = next.value();
    }
    for (const auto& u : seed.transfer.uploads)
        if (static_cast<int>(u.state) < 5 || u.cleanupPending) {
            auto next = reserved.add(u.original.length.value());
            if (!next) return next.error();
            reserved = next.value();
        }
    if (retained != seed.transfer.storage.retained || reserved != seed.transfer.storage.reserved) return ErrorCode::IntegrityError;
    std::error_code ec;
    for (auto suffix : { "", "-wal", "-shm", "-journal" }) std::filesystem::remove(databasePath().string() + suffix, ec);
    auto made = createSqliteDatabase(databasePath(), { 0 });
    if (!made) return made.error();
    auto c = openSqliteDatabase(databasePath(), { 0 });
    if (!c) return c.error();
    bool active = false;
    SqliteTransactionContext ctx(*c.value(), active);
    auto begun = ctx.begin();
    if (!begun) return begun.error();
    auto seeded = seedDriveMetadata(*c.value(), seed);
    if (!seeded) return seeded.error();
    SqliteTrashRepository trash(ctx);
    for (const auto& e : seed.trashEntries) {
        auto r = trash.insert(e);
        if (!r) return r.error();
    }
    const auto& s = seed.transfer.storage;
    auto r = detail::one(ctx, detail::write(ctx, "INSERT INTO storage_accounting VALUES(1,?1,?2,?3,?4)", { s.budget.value(), s.retained.value(), s.reserved.value(), s.revision.value() }));
    if (!r) return r.error();
    const auto& n = seed.nodeAccounting;
    r = detail::one(ctx, detail::write(ctx, "UPDATE node_accounting SET used_nodes=?1,reserved_nodes=?2,revision=?3 WHERE singleton=1", { n.used.value(), n.reserved.value(), n.revision.value() }));
    if (!r) return r.error();
    for (const auto& u : seed.transfer.uploads) {
        r = seedTransferUpload(ctx, u);
        if (!r) return r.error();
    }
    auto consistent = detail::query(ctx, "SELECT NOT EXISTS(SELECT 1 FROM blobs b WHERE (b.state=0 AND ((b.unreferenced_at IS NULL)<>EXISTS(SELECT 1 FROM nodes n WHERE n.blob_id=b.id))) OR (b.state=1 AND (EXISTS(SELECT 1 FROM nodes n WHERE n.blob_id=b.id) OR EXISTS(SELECT 1 FROM uploads u WHERE u.pending_blob_id=b.id AND (u.state IN (3,4) OR u.cleanup_pending=1)))))");
    if (!consistent) return consistent.error();
    auto row = consistent.value()->step();
    if (!row || !row.value() || consistent.value()->integer(0).value() != 1) return ErrorCode::IntegrityError;
    return ctx.commit();
}
Result<std::unique_ptr<DriveMetadataStore>> SqliteTestDriveBackend::connect() {
    auto c = openSqliteDatabase(databasePath(), { 0 });
    if (!c) return c.error();
    return std::unique_ptr<DriveMetadataStore>(new detail::MetadataStoreImpl(std::move(c).value(), { 8192, 1, 1 }));
}
Result<std::vector<DriveEvent>> SqliteTestDriveBackend::readDriveAudit() {
    auto c = openSqliteDatabase(databasePath(), { 0 });
    if (!c) return c.error();
    bool active = false;
    SqliteTransactionContext ctx(*c.value(), active);
    auto begin = ctx.beginRead();
    if (!begin) return begin.error();
    auto q = detail::query(ctx, "SELECT request_id,observed_at,actor_id,maintenance,kind,first_id,second_id,third_id FROM drive_audit_events ORDER BY sequence");
    if (!q) return q.error();
    std::vector<DriveEvent> out;
    for (;;) {
        auto row = q.value()->step();
        if (!row) return row.error();
        if (!row.value()) return out;
        auto& s = *q.value();
        auto request = readId<RequestId>(s, 0);
        auto at = readTime(s, 1);
        auto maintenance = readBoolean(s, 3);
        auto kind = s.integer(4);
        auto first = s.text(5);
        if (!request || !at || !maintenance || !kind || !first) return ErrorCode::IntegrityError;
        DriveActor actor = MaintenanceActor{};
        if (!maintenance.value()) {
            auto id = readId<UserId>(s, 2);
            if (!id) return id.error();
            actor = id.value();
        }
        auto second = s.isNull(6).value() ? std::string{} : s.text(6).value();
        auto third = s.isNull(7).value() ? std::string{} : s.text(7).value();
        DriveAction action = NodeRenamed{ NodeId::parse(first.value()).value() };
        switch (kind.value()) {
        case 0:
            break;
        case 1:
            action = NodeMoved{ NodeId::parse(first.value()).value(), NodeId::parse(second).value(), NodeId::parse(third).value() };
            break;
        case 2:
            action = FileCopied{ NodeId::parse(first.value()).value(), NodeId::parse(second).value() };
            break;
        case 3:
            action = TreeTrashed{ TrashId::parse(first.value()).value(), NodeId::parse(second).value() };
            break;
        case 4:
            action = TreeRestored{ TrashId::parse(first.value()).value(), NodeId::parse(second).value() };
            break;
        case 5:
            action = TreePurged{ TrashId::parse(first.value()).value(), NodeId::parse(second).value() };
            break;
        case 6:
            action = GcMarked{ BlobId::parse(first.value()).value() };
            break;
        case 7:
            action = GcFinished{ BlobId::parse(first.value()).value() };
            break;
        default:
            return ErrorCode::IntegrityError;
        }
        out.push_back({ request.value(), at.value(), actor, action });
    }
}
}  // namespace testing
}  // namespace anof::infrastructure::sqlite
namespace anof::testing {
std::unique_ptr<TestDriveBackend> makeTestDriveBackend() { return std::make_unique<infrastructure::sqlite::testing::SqliteTestDriveBackend>(); }
}  // namespace anof::testing
