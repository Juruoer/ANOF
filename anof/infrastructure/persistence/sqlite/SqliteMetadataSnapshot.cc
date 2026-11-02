#include <anof/infrastructure/persistence/sqlite/SqliteInitializedState.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceFiles.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataSnapshot.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <anof/infrastructure/persistence/sqlite/internal/MaintenanceSql.h>
#include <anof/infrastructure/system/internal/MaintenanceFiles.h>
#include <fcntl.h>
#include <sodium.h>
#include <sqlite3.h>
#include <unistd.h>
namespace anof::infrastructure::sqlite {
namespace {
using namespace maintenance;
Result<std::unique_ptr<SqliteConnection>> copyDatabase(const MaintenanceDestination& d) {
    auto destination = system::maintenance::destination(d);
    if (!destination || destination.value()->failed || destination.value()->sealed) return ErrorCode::IntegrityError;
    auto root = system::detail::openInstanceRoot(d.staging());
    if (!root) return root.error();
    auto file = system::detail::openPrivateFile(root.value().rootFd.get(), "metadata.sqlite3", true);
    if (!file) return file.error();
    file.value() = system::detail::OwnedFd{};
    return SqliteConnection::open(d.staging() / "metadata.sqlite3", {}, SqliteOpenMode::MaintenanceCopy);
}
class Snapshot final : public MetadataSnapshot {
    PasswordHashLimits limits_;

public:
    explicit Snapshot(PasswordHashLimits limits) : limits_(limits) {}

private:
    MaintenanceResult<void> exportConsistent(const MaintenanceContext& c, const InstanceLease& lease, const MaintenanceDestination& d) override {
        auto destination = system::maintenance::destination(d);
        if (!destination || destination.value()->sealed || destination.value()->failed) return failure(c, ErrorCode::IntegrityError, MaintenanceStage::Export);
        auto source = detail::openInstanceDatabase(lease, {}, false);
        if (!source) return failure(c, ErrorCode::IntegrityError, MaintenanceStage::Export);
        auto root = system::detail::openInstanceRoot(d.staging());
        if (!root) return failure(c, root.error().code, MaintenanceStage::Export);
        system::detail::OwnedFd file(::openat(root.value().rootFd.get(), "metadata.sqlite3", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (file.get() < 0) return failure(c, system::detail::filesystemError(errno), MaintenanceStage::Export);
        file = system::detail::OwnedFd{};
        auto dest = SqliteConnection::open(d.staging() / "metadata.sqlite3", {}, SqliteOpenMode::MaintenanceCopy);
        if (!dest) return failure(c, dest.error().code, MaintenanceStage::Export);
        auto* backup = sqlite3_backup_init(dest.value()->handle(), "main", source.value()->connection->handle(), "main");
        if (!backup) return failure(c, ErrorCode::InternalError, MaintenanceStage::Export);
        int code = SQLITE_OK;
        do {
            if (c.cancelled.load()) {
                sqlite3_backup_finish(backup);
                return MaintenanceFailure{ MaintenanceError::Cancelled, MaintenanceStage::Export, c.operationId, MaintenanceOutcome::NotStarted, MaintenanceReportStatus::Incomplete, {}, {} };
            }
            code = sqlite3_backup_step(backup, 128);
        } while (code == SQLITE_OK);
        auto finished = sqlite3_backup_finish(backup);
        if (code != SQLITE_DONE || finished != SQLITE_OK) return failure(c, ErrorCode::InternalError, MaintenanceStage::Export);
        return {};
    }
    MaintenanceResult<BackupExclusions> normalizeBackup(const MaintenanceContext& c, const MaintenanceDestination& d) override {
        auto db = copyDatabase(d);
        if (!db) return failure(c, db.error().code, MaintenanceStage::Normalize);
        auto schema = verifySqliteSchema(*db.value());
        if (!schema) return failure(c, schema.error().code, MaintenanceStage::Normalize);
        bool active = false;
        SqliteTransactionContext ctx(*db.value(), active);
        auto begun = ctx.begin();
        if (!begun) return failure(c, begun.error().code, MaintenanceStage::Normalize);
        auto q = detail::query(ctx, "SELECT (SELECT count(*) FROM quarantine_entries),(SELECT coalesce(sum(charged_bytes),0) FROM quarantine_entries),(SELECT count(*) FROM blobs b WHERE NOT EXISTS(SELECT 1 FROM nodes n WHERE n.blob_id=b.id)),(SELECT coalesce(sum(charged_bytes),0) FROM blobs b WHERE NOT EXISTS(SELECT 1 FROM nodes n WHERE n.blob_id=b.id)),(SELECT count(*) FROM uploads),EXISTS(SELECT 1 FROM blobs WHERE state=2),EXISTS(SELECT 1 FROM maintenance_runs WHERE completed=0),EXISTS(SELECT 1 FROM blobs b JOIN nodes n ON n.blob_id=b.id WHERE b.state<>0)");
        if (!q) return failure(c, q.error().code, MaintenanceStage::Normalize);
        auto row = q.value()->step();
        if (!row || !row.value()) return failure(c, ErrorCode::IntegrityError, MaintenanceStage::Normalize);
        auto& s = *q.value();
        if (s.integer(5).value() || s.integer(7).value()) return failure(c, ErrorCode::IntegrityError, MaintenanceStage::Normalize);
        if (s.integer(6).value()) return MaintenanceFailure{ MaintenanceError::MaintenancePending, MaintenanceStage::Normalize, c.operationId, MaintenanceOutcome::NotStarted, MaintenanceReportStatus::Incomplete, {}, {} };
        BackupExclusions out{ readQuantity<NodeCount>(s, 0).value(), readQuantity<ByteCount>(s, 1).value(), readQuantity<NodeCount>(s, 2).value(), readQuantity<ByteCount>(s, 3).value(), readQuantity<NodeCount>(s, 4).value() };
        q.value().reset();
        auto trigger = detail::query(ctx, "SELECT sql FROM sqlite_schema WHERE type='trigger' AND name='blobs_no_referenced_delete'");
        if (!trigger) return failure(c, trigger.error().code);
        auto present = trigger.value()->step();
        if (!present || !present.value()) return failure(c, ErrorCode::IntegrityError);
        auto sql = trigger.value()->text(0);
        if (!sql) return failure(c, sql.error().code);
        trigger.value().reset();
        auto deleted = db.value()->execute("DELETE FROM sessions;DELETE FROM uploads;DELETE FROM invites;DELETE FROM quarantine_entries;DELETE FROM maintenance_actions;DELETE FROM maintenance_runs;DROP TRIGGER blobs_no_referenced_delete;DELETE FROM blobs WHERE NOT EXISTS(SELECT 1 FROM nodes WHERE blob_id=blobs.id)");
        if (!deleted) return failure(c, deleted.error().code, MaintenanceStage::Normalize);
        auto restore = db.value()->execute(sql.value());
        if (!restore) return failure(c, restore.error().code, MaintenanceStage::Normalize);
        auto ledger = accountingDigest(ctx, true);
        if (!ledger) return failure(c, ledger.error().code, MaintenanceStage::Normalize);
        auto committed = ctx.commit();
        if (!committed) return failure(c, committed.error().code, MaintenanceStage::Normalize);
        return out;
    }
    MaintenanceResult<BackupFileDigest> verifyBackup(const MaintenanceContext& c, const MaintenanceDestination& d) override {
        auto db = copyDatabase(d);
        if (!db) return failure(c, db.error().code, MaintenanceStage::Verify);
        if (!verifySqliteSchema(*db.value()) || !detail::verifyInitialized(*db.value(), limits_)) return failure(c, ErrorCode::IntegrityError, MaintenanceStage::Verify);
        {
            bool active = false;
            SqliteTransactionContext transaction(*db.value(), active);
            auto begun = transaction.beginRead();
            if (!begun || !accountingDigest(transaction, false) || !verifyRecordedAccounting(transaction)) return failure(c, ErrorCode::IntegrityError, MaintenanceStage::Verify);
            // 读事务由上下文析构结束；写事务 commit 不适用于只读快照。
        }
        auto check = SqliteStatement::prepare(*db.value(), "SELECT NOT EXISTS(SELECT 1 FROM sessions) AND NOT EXISTS(SELECT 1 FROM uploads) AND NOT EXISTS(SELECT 1 FROM invites) AND NOT EXISTS(SELECT 1 FROM quarantine_entries) AND NOT EXISTS(SELECT 1 FROM maintenance_runs) AND NOT EXISTS(SELECT 1 FROM blobs b WHERE state<>0 OR NOT EXISTS(SELECT 1 FROM nodes n WHERE n.blob_id=b.id))");
        if (!check) return failure(c, check.error().code);
        auto row = check.value()->step();
        if (!row || !row.value() || check.value()->integer(0).value() != 1) return failure(c, ErrorCode::IntegrityError);
        check.value().reset();
        auto journal = db.value()->execute("PRAGMA wal_checkpoint(TRUNCATE);PRAGMA journal_mode=DELETE");
        if (!journal) return failure(c, journal.error().code);
        db.value().reset();
        auto root = system::detail::openInstanceRoot(d.staging());
        if (!root) return failure(c, root.error().code);
        for (const auto* suffix : { "metadata.sqlite3-wal", "metadata.sqlite3-shm", "metadata.sqlite3-journal" }) {
            struct stat st{};
            if (::fstatat(root.value().rootFd.get(), suffix, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) return failure(c, ErrorCode::IntegrityError);
        }
        auto file = system::detail::openPrivateFile(root.value().rootFd.get(), "metadata.sqlite3", true);
        if (!file) return failure(c, file.error().code);
        if (::fsync(file.value().get())) return failure(c, ErrorCode::InternalError);
        crypto_hash_sha256_state hash;
        crypto_hash_sha256_init(&hash);
        std::array<unsigned char, kMaintenanceBlockBytes> bytes{};
        ByteCount length;
        for (;;) {
            if (c.cancelled.load()) return MaintenanceFailure{ MaintenanceError::Cancelled, MaintenanceStage::Verify, c.operationId, MaintenanceOutcome::NotStarted, MaintenanceReportStatus::Incomplete, {}, {} };
            auto n = ::read(file.value().get(), bytes.data(), bytes.size());
            if (n < 0) {
                if (errno == EINTR) continue;
                return failure(c, ErrorCode::InternalError);
            }
            if (!n) break;
            auto sum = length.add(n);
            if (!sum) return failure(c, sum.error().code);
            length = sum.value();
            crypto_hash_sha256_update(&hash, bytes.data(), static_cast<unsigned long long>(n));
        }
        std::array<std::uint8_t, 32> digest{};
        crypto_hash_sha256_final(&hash, digest.data());
        return BackupFileDigest{ length, Sha256(digest) };
    }
    MaintenanceResult<void> prepareRestore(const MaintenanceContext& c, const MaintenanceDestination& d, InstanceId id) override {
        auto verified = verifyBackup(c, d);
        if (!verified) return verified.error();
        auto db = copyDatabase(d);
        if (!db) return failure(c, db.error().code);
        auto q = SqliteStatement::prepare(*db.value(), "SELECT instance_id FROM instance_settings WHERE singleton=1");
        if (!q) return failure(c, q.error().code);
        auto row = q.value()->step();
        if (!row || !row.value() || q.value()->text(0).value() != id.toString()) return failure(c, ErrorCode::IntegrityError);
        q.value().reset();
        auto wal = db.value()->execute("PRAGMA journal_mode=WAL");
        if (!wal) return failure(c, wal.error().code);
        db.value().reset();
        auto destination = system::maintenance::destination(d);
        if (!destination) return failure(c, destination.error().code);
        destination.value()->restorePrepared = id;
        return {};
    }
};
}  // namespace
Result<std::unique_ptr<MetadataSnapshot>> makeSqliteMetadataSnapshot(PasswordHashLimits limits) {
    if (!detail::validPasswordLimits(limits)) return ErrorCode::InvalidArgument;
    return std::unique_ptr<MetadataSnapshot>(new Snapshot(limits));
}
}  // namespace anof::infrastructure::sqlite
