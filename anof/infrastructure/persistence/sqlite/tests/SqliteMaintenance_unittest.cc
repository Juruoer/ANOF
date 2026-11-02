#define BOOST_TEST_MODULE SqliteMaintenance
#include <anof/infrastructure/config/internal/ConfigurationText.h>
#include <anof/infrastructure/persistence/sqlite/SqliteAuditRetention.h>
#include <anof/infrastructure/persistence/sqlite/SqliteBlobRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataInspection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataRepair.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataSnapshot.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStorageRepository.h>
#include <anof/infrastructure/persistence/sqlite/tests/BootstrapFixture.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteFaultVfs.h>
#include <anof/infrastructure/storage/local/LocalMaintenanceFileStore.h>
#include <anof/infrastructure/storage/local/internal/MaintenanceFileTesting.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/infrastructure/system/LinuxOfflineInstanceAccess.h>
#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <sys/stat.h>

#include <boost/test/unit_test.hpp>
#include <fstream>
using namespace anof;
using namespace anof::infrastructure;
using namespace anof::infrastructure::sqlite;
using namespace anof::infrastructure::sqlite::testing;
namespace {
template <class T>
T id(unsigned n) { return bootstrapId<T>(n); }
Sha256 digest() { return Sha256::parse(std::string(64, 'a')).value(); }
UtcTime now() { return UtcTime{ std::chrono::microseconds{ 1000 } }; }
struct Fixture {
    std::filesystem::path base, root, work;
    std::atomic<bool> cancelled{ false };
    MaintenanceContext call{ id<OperationId>(9), cancelled };
    std::unique_ptr<InstanceLease> lease;
    std::unique_ptr<OfflineInstanceAccess> access;
    std::unique_ptr<MaintenanceFileStore> files;
    Fixture() {
        char name[] = "/tmp/anof-m5-maintenance-XXXXXX";
        base = mkdtemp(name);
        root = base / "source";
        work = base / "work";
        for (const auto& p : { root, work, root / ".anof", root / ".anof/objects", root / ".anof/objects/12", root / ".anof/uploads", root / ".anof/quarantine" }) BOOST_REQUIRE(::mkdir(p.c_str(), 0700) == 0);
        write(root / ".anof/instance.lock", "");
        write(root / ".anof/instance.json", system::detail::formatText(bootstrapSeed().identity));
        InstanceConfiguration config;
        config.developmentHttp = true;
        write(root / ".anof/config.toml", config::detail::serializeConfiguration(config).value());
        createBootstrappedTestDatabase(root / ".anof/metadata.sqlite3");
        access = std::move(system::makeLinuxOfflineInstanceAccess()).value();
        files = std::move(storage::makeLocalMaintenanceFileStore()).value();
        auto locked = access->acquireExisting(call, root);
        BOOST_REQUIRE(locked);
        lease = std::move(locked).value();
    }
    ~Fixture() {
        lease.reset();
        std::error_code ignored;
        std::filesystem::remove_all(base, ignored);
    }
    void write(const std::filesystem::path& p, const std::string& text) {
        std::ofstream out(p, std::ios::binary);
        out << text;
        out.close();
        BOOST_REQUIRE(out.good());
        BOOST_REQUIRE(::chmod(p.c_str(), 0600) == 0);
    }
    std::unique_ptr<SqliteConnection> db() {
        auto c = openSqliteDatabase(root / ".anof/metadata.sqlite3");
        BOOST_REQUIRE(c);
        return std::move(c).value();
    }
    std::int64_t scalar(const std::string& sql) {
        auto c = db();
        auto q = SqliteStatement::prepare(*c, sql);
        BOOST_REQUIRE(q);
        BOOST_REQUIRE(q.value()->step().value());
        return q.value()->integer(0).value();
    }
};
void checkObjectsSyncFailure(Fixture&, MetadataSnapshot&, MaintenanceBackupSource&);
void checkBackupContext(Fixture&, const std::filesystem::path&);
}  // namespace
BOOST_AUTO_TEST_CASE(existing_lock_is_noncreating_and_capture_preserves_wal_source) {
    Fixture f;
    auto busy = f.access->acquireExisting(f.call, f.root);
    BOOST_REQUIRE(!busy);
    BOOST_CHECK(busy.error().code == MaintenanceError::Busy);
    auto source = f.db();
    BOOST_REQUIRE(source->execute("PRAGMA wal_autocheckpoint=0; UPDATE storage_accounting SET content_budget_bytes=2048,revision=2"));
    auto capture = f.access->captureInspection(f.call, *f.lease, f.work);
    BOOST_REQUIRE(capture);
    auto inspect = makeSqliteMetadataInspection();
    BOOST_REQUIRE(inspect);
    auto reader = inspect.value()->open(f.call, *capture.value());
    BOOST_REQUIRE(reader);
    auto account = reader.value()->accounting(f.call);
    BOOST_REQUIRE(account);
    BOOST_CHECK_EQUAL(account.value().recordedStorage.budget.value(), 2048);
    auto second = f.access->captureInspection(f.call, *f.lease, f.base);
    BOOST_REQUIRE(second);
    BOOST_CHECK(capture.value()->fingerprint().metadata == second.value()->fingerprint().metadata);
    BOOST_CHECK(capture.value()->fingerprint().inventory == second.value()->fingerprint().inventory);
    reader.value().reset();
    capture.value().reset();
    second.value().reset();
    source.reset();
    f.lease.reset();
    std::filesystem::remove(f.root / ".anof/instance.lock");
    auto missing = f.access->acquireExisting(f.call, f.root);
    BOOST_CHECK(!missing);
    BOOST_CHECK(!std::filesystem::exists(f.root / ".anof/instance.lock"));
}
BOOST_AUTO_TEST_CASE(quarantine_receipts_bind_run_revision_and_charge_once) {
    Fixture f;
    const auto blob = id<BlobId>(8);
    f.write(f.root / ".anof/objects/12" / blob.toString(), "test");
    auto observed = f.files->observe(f.call, *f.lease, blob, InspectionMode::Full);
    BOOST_REQUIRE(observed && observed.value());
    auto repair = makeSqliteMetadataRepair();
    BOOST_REQUIRE(repair);
    auto session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    MaintenanceRun run{ id<MaintenanceRunId>(10), id<MaintenancePlanId>(11), bootstrapSeed().identity.instanceId, digest(), now(), now(), false, {} };
    QuarantineOrphan payload{ blob, id<QuarantineId>(13), observed.value()->evidence, *observed.value()->sha256 };
    MaintenanceAction action{ id<MaintenanceActionId>(14), payload, digest() };
    MaintenanceActionRecord record{ {}, action, MaintenanceActionState::Prepared, {}, now(), now(), {} };
    QuarantineEntry entry{ payload.destination, payload.source, payload.before, payload.sha256, payload.before.size, QuarantineState::Prepared, run.plan, action.id, now(), now(), {} };
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->insertRun(f.call, run));
        BOOST_REQUIRE(tx.value()->prepareAction(f.call, run.id, record));
        BOOST_REQUIRE(tx.value()->prepareQuarantine(f.call, entry));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    BOOST_CHECK_EQUAL(f.scalar("SELECT retained_bytes FROM storage_accounting"), 4);
    BOOST_REQUIRE(f.files->quarantine(f.call, *f.lease, entry));
    MaintenanceEvidenceBinding binding{ run.instance, run.id, run.plan, run.planDigest, action.id, {} };
    auto receipt = f.files->verifyAction(f.call, *f.lease, binding, action);
    BOOST_REQUIRE(receipt);
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        auto wrong = receipt.value();
        wrong.binding.plan = id<MaintenancePlanId>(15);
        BOOST_CHECK(!tx.value()->holdQuarantine(f.call, entry.id, {}, wrong, now()));
        BOOST_CHECK(!tx.value()->commit(f.call));
    }
    BOOST_CHECK_EQUAL(f.scalar("SELECT state FROM quarantine_entries"), 0);
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->holdQuarantine(f.call, entry.id, {}, receipt.value(), now()));
        BOOST_REQUIRE(tx.value()->advanceAction(f.call, run.id, action.id, {}, MaintenanceActionState::Applied, receipt.value(), now()));
        BOOST_REQUIRE(tx.value()->appendAudit(f.call, { run.id, f.call.operationId, MaintenanceAuditKind::ActionApplied, MaintenanceOutcome::Confirmed, {}, entry.id, action.id, now() }));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    session.value().reset();
    session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_CHECK(!tx.value()->advanceAction(f.call, run.id, action.id, Revision::from(2).value(), MaintenanceActionState::Verified, receipt.value(), now()));
        BOOST_CHECK(!tx.value()->commit(f.call));
    }
    binding.actionRevision = Revision::from(2).value();
    receipt = f.files->verifyAction(f.call, *f.lease, binding, action);
    BOOST_REQUIRE(receipt);
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->advanceAction(f.call, run.id, action.id, binding.actionRevision, MaintenanceActionState::Verified, receipt.value(), now()));
        BOOST_REQUIRE(tx.value()->completeRun(f.call, run.id, {}, now()));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    BOOST_CHECK_EQUAL(f.scalar("SELECT retained_bytes FROM storage_accounting"), 4);
    auto rows = session.value()->actions(f.call, run.id, PageSize::from(1).value(), {});
    BOOST_REQUIRE(rows);
    BOOST_REQUIRE_EQUAL(rows.value().items.size(), 1);
    BOOST_REQUIRE(rows.value().items[0].resultEvidence);
    BOOST_CHECK_EQUAL(rows.value().items[0].revision.value(), 3);
    auto retention = openSqliteAuditRetention(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(retention);
    BOOST_REQUIRE(retention.value()->eraseCompletedRuns(UtcTime{ std::chrono::seconds{ 100000000 } }, AuditBatchSize::from(1).value()));
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM maintenance_runs"), 1);
}
BOOST_AUTO_TEST_CASE(normalized_backup_is_sealed_verified_and_never_overwrites) {
    Fixture f;
    auto destination = f.access->acquireDestination(f.call, f.root, f.base / "backup");
    BOOST_REQUIRE(destination);
    auto snapshot = makeSqliteMetadataSnapshot(bootstrapBudget);
    BOOST_REQUIRE(snapshot);
    BOOST_REQUIRE(snapshot.value()->exportConsistent(f.call, *f.lease, *destination.value()));
    auto excluded = snapshot.value()->normalizeBackup(f.call, *destination.value());
    BOOST_REQUIRE(excluded);
    auto database = snapshot.value()->verifyBackup(f.call, *destination.value());
    BOOST_REQUIRE(database);
    InstanceConfiguration config;
    config.developmentHttp = true;
    auto configuration = f.files->writeConfiguration(f.call, *destination.value(), config);
    BOOST_REQUIRE(configuration);
    auto index = f.files->finishObjectIndex(f.call, *destination.value());
    BOOST_REQUIRE(index);
    BackupManifest manifest{ 1, "sqlite", "test", 5, 5, id<BackupId>(15), bootstrapSeed().identity.instanceId, now(), database.value(), configuration.value(), index.value(), excluded.value() };
    BOOST_REQUIRE(f.files->sealBackup(f.call, *destination.value(), manifest));
    BOOST_REQUIRE(f.files->publish(f.call, *destination.value()));
    auto opened = f.files->openBackup(f.call, f.base / "backup");
    BOOST_REQUIRE(opened);
    BOOST_CHECK_EQUAL(opened.value()->manifest().objects.count.value(), 0);
    checkObjectsSyncFailure(f, *snapshot.value(), *opened.value());
    checkBackupContext(f, f.base / "backup");
    auto existing = f.access->acquireDestination(f.call, f.root, f.base / "backup");
    BOOST_CHECK(!existing);
    f.write(f.base / "backup/extra", "unexpected");
    BOOST_CHECK(!f.files->openBackup(f.call, f.base / "backup"));
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM users"), 1);
}

BOOST_AUTO_TEST_CASE(unavailable_charge_evidence_and_rollback_remain_after_reopen) {
    Fixture f;
    const auto blob = id<BlobId>(8);
    f.write(f.root / ".anof/objects/12" / blob.toString(), "test");
    auto original = f.files->observe(f.call, *f.lease, blob, InspectionMode::Full);
    BOOST_REQUIRE(original && original.value());
    {
        auto db = f.db();
        BOOST_REQUIRE(db->execute("INSERT INTO blobs(id,size,sha256,state,created_at,updated_at,revision,unreferenced_at,charged_bytes) VALUES('" + blob.toString() + "',4,x'" + original.value()->sha256->toString() + "',0,1000,1000,1,1000,4);UPDATE storage_accounting SET retained_bytes=4,content_budget_bytes=4,revision=2"));
    }
    f.write(f.root / ".anof/objects/12" / blob.toString(), "longer!");
    auto grown = f.files->observe(f.call, *f.lease, blob, InspectionMode::Quick);
    BOOST_REQUIRE(grown && grown.value());
    auto repair = makeSqliteMetadataRepair();
    auto session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    MaintenanceRun run{ id<MaintenanceRunId>(10), id<MaintenancePlanId>(11), bootstrapSeed().identity.instanceId, digest(), now(), now(), false, {} };
    MarkBlobUnavailable payload{ blob, {}, { UnavailableReason::Length, grown.value()->evidence, {} } };
    MaintenanceAction action{ id<MaintenanceActionId>(14), payload, digest() };
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->insertRun(f.call, run));
        BOOST_REQUIRE(tx.value()->prepareAction(f.call, run.id, { {}, action, MaintenanceActionState::Prepared, {}, now(), now(), {} }));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    MaintenanceEvidenceBinding binding{ run.instance, run.id, run.plan, run.planDigest, action.id, {} };
    auto receipt = f.files->verifyAction(f.call, *f.lease, binding, action);
    BOOST_REQUIRE(receipt);
    for (int fault = 0; fault < 14; ++fault) {
        auto invalid = receipt.value();
        switch (fault) {
        case 0:
            invalid.binding.instance = id<InstanceId>(1);
            break;
        case 1:
            invalid.binding.run = id<MaintenanceRunId>(1);
            break;
        case 2:
            invalid.binding.plan = id<MaintenancePlanId>(1);
            break;
        case 3:
            invalid.binding.planDigest = Sha256::parse(std::string(64, 'b')).value();
            break;
        case 4:
            invalid.binding.action = id<MaintenanceActionId>(1);
            break;
        case 5:
            invalid.binding.actionRevision = Revision::from(2).value();
            break;
        case 6:
            invalid.quality = MaintenanceEvidenceQuality::Unknown;
            break;
        case 7:
            invalid.files.clear();
            break;
        case 8:
            invalid.files.push_back(invalid.files[0]);
            break;
        case 9:
            invalid.files[0].key = id<BlobId>(1);
            break;
        case 10:
            invalid.files[0].file->size = ByteCount::from(8).value();
            break;
        case 11:
            ++invalid.files[0].file->inode;
            break;
        case 12:
            invalid.files[0].sha256 = digest();
            break;
        case 13:
            invalid.quality = MaintenanceEvidenceQuality::Observed;
            break;
        }
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_CHECK(!tx.value()->markUnavailable(f.call, payload, invalid, now()));
        BOOST_CHECK(!tx.value()->commit(f.call));
    }
    BOOST_CHECK_EQUAL(f.scalar("SELECT retained_bytes FROM storage_accounting"), 4);
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->markUnavailable(f.call, payload, receipt.value(), now()));
        BOOST_REQUIRE(tx.value()->advanceAction(f.call, run.id, action.id, {}, MaintenanceActionState::Applied, receipt.value(), now()));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    session.value().reset();
    BOOST_CHECK_EQUAL(f.scalar("SELECT retained_bytes FROM storage_accounting"), 7);
    BOOST_CHECK_EQUAL(f.scalar("SELECT charged_bytes FROM blobs"), 7);
    BOOST_CHECK_EQUAL(f.scalar("SELECT state FROM blobs"), 2);
    session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    binding.actionRevision = Revision::from(2).value();
    receipt = f.files->verifyAction(f.call, *f.lease, binding, action);
    BOOST_REQUIRE(receipt);
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->advanceAction(f.call, run.id, action.id, binding.actionRevision, MaintenanceActionState::Verified, receipt.value(), now()));
        BOOST_REQUIRE(tx.value()->completeRun(f.call, run.id, {}, now()));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    auto db = f.db();
    bool active = false;
    SqliteTransactionContext ctx(*db, active);
    BOOST_REQUIRE(ctx.begin());
    SqliteBlobRepository blobs(ctx);
    auto record = blobs.record(blob);
    BOOST_REQUIRE(record && record.value());
    BOOST_CHECK(record.value()->unavailable.has_value());
    auto stats = blobs.gcStatistics();
    BOOST_REQUIRE(stats);
    BOOST_CHECK_EQUAL(stats.value().unavailableBytes.value(), 7);
    BOOST_CHECK_EQUAL(stats.value().pendingBytes.value(), 0);
    auto candidates = blobs.gcCandidates(UtcTime{ std::chrono::seconds{ 10000000 } }, PageSize::from(16).value(), {});
    BOOST_REQUIRE(candidates);
    BOOST_CHECK(candidates.value().items.empty());
    SqliteStorageRepository storage(ctx);
    BOOST_CHECK(!storage.reserve(ByteCount::from(1).value()));
    BOOST_CHECK(!ctx.commit());
}

BOOST_AUTO_TEST_CASE(audit_cleanup_uses_one_total_budget_and_keeps_startup_valid) {
    Fixture f;
    {
        auto db = f.db();
        for (int i = 0; i < 3; ++i) BOOST_REQUIRE(db->execute("INSERT INTO transfer_audit_events(kind,request_id,actor_id,upload_id,occurred_at,failure) VALUES(6,'" + id<RequestId>(1).toString() + "','" + bootstrapSeed().adminUserId.toString() + "',NULL," + std::to_string(124 + i) + ",NULL)"));
    }
    auto retention = openSqliteAuditRetention(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(retention);
    auto first = retention.value()->eraseExpired({ now(), AuditBatchSize::from(2).value() });
    BOOST_REQUIRE(first);
    BOOST_CHECK_EQUAL(first.value().removed.value(), 2);
    BOOST_CHECK(first.value().more);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM identity_audit_events"), 0);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM transfer_audit_events"), 2);
    auto second = retention.value()->eraseExpired({ now(), AuditBatchSize::from(2).value() });
    BOOST_REQUIRE(second);
    BOOST_CHECK_EQUAL(second.value().removed.value(), 2);
    BOOST_CHECK(!second.value().more);
    auto store = openSqliteDriveMetadataStore(f.root / ".anof/metadata.sqlite3", { bootstrapBudget, {} });
    BOOST_REQUIRE(store);
}

namespace {
struct FaultIo final : system::maintenance::Io {
    enum class Point { None,
                       Read,
                       Write,
                       Sync,
                       Truncate,
                       Rename };
    Point point = Point::None;
    int error = EIO;
    bool renamed = false;
    ssize_t read(int fd, void* data, std::size_t size) override {
        if (point == Point::Read) {
            errno = error;
            return -1;
        }
        return Io::read(fd, data, size);
    }
    ssize_t write(int fd, const void* data, std::size_t size) override {
        if (point == Point::Write) {
            errno = error;
            return -1;
        }
        return Io::write(fd, data, size);
    }
    int sync(int fd) override {
        if (point == Point::Sync) {
            errno = error;
            return -1;
        }
        return Io::sync(fd);
    }
    int truncate(int fd, off_t size) override {
        if (point == Point::Truncate) {
            errno = error;
            return -1;
        }
        return Io::truncate(fd, size);
    }
    int rename(int from, const char* name, int to, const char* target) override {
        if (point == Point::Rename) {
            errno = error;
            return -1;
        }
        auto r = Io::rename(from, name, to, target);
        if (r == 0) {
            renamed = true;
            if (error == ENOSPC) point = Point::Sync;
        }
        return r;
    }
};
struct ObjectsSyncFailure final : system::maintenance::Io {
    std::filesystem::path objects;
    bool triggered = false;
    bool shardSynced = false;
    int sync(int fd) override {
        std::error_code error;
        auto path = std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(fd), error);
        struct stat status{};
        if (!error && ::fstat(fd, &status) == 0 && S_ISDIR(status.st_mode)) {
            if (path.parent_path() == objects) shardSynced = true;
            if (path == objects) {
                triggered = true;
                errno = EIO;
                return -1;
            }
        }
        return Io::sync(fd);
    }
};
void checkObjectsSyncFailure(Fixture& f, MetadataSnapshot& snapshot, MaintenanceBackupSource& backup) {
    MaintenanceContext call{ id<OperationId>(7), f.cancelled };
    auto destination = f.access->acquireDestination(call, f.root, f.base / "failed-objects-sync");
    BOOST_REQUIRE(destination);
    const auto backupPath = f.base / (backup.manifest().objects.count.value() ? "shared-backup" : "backup");
    auto reopened = f.files->openBackup(call, backupPath);
    BOOST_REQUIRE(reopened);
    BOOST_REQUIRE(f.files->copyBackup(call, *reopened.value(), *destination.value()));
    InstanceConfiguration configuration;
    configuration.developmentHttp = true;
    BOOST_REQUIRE(f.files->writeConfiguration(call, *destination.value(), configuration));
    BOOST_REQUIRE(snapshot.prepareRestore(call, *destination.value(), backup.manifest().instance));
    auto io = std::make_shared<ObjectsSyncFailure>();
    io->objects = destination.value()->staging() / "objects";
    auto files = storage::detail::makeLocalMaintenanceFileStore(io);
    BOOST_REQUIRE(files);
    auto sealed = files.value()->sealRestoredInstance(call, *destination.value(), backup.manifest().instance);
    BOOST_REQUIRE(!sealed);
    BOOST_CHECK(io->triggered);
    BOOST_CHECK_EQUAL(io->shardSynced, backup.manifest().objects.count.value() != 0);
    BOOST_CHECK(sealed.error().code == MaintenanceError::PublicationOutcomeUnknown);
    BOOST_CHECK(sealed.error().outcome == MaintenanceOutcome::Unknown);
    BOOST_CHECK(!files.value()->publish(call, *destination.value()));
    BOOST_CHECK(std::filesystem::exists(destination.value()->staging() / "objects"));
    BOOST_CHECK(std::filesystem::exists(destination.value()->staging() / "metadata.sqlite3"));
    BOOST_CHECK(!std::filesystem::exists(destination.value()->target()));
}
}  // namespace
BOOST_AUTO_TEST_CASE(file_failures_do_not_issue_durable_receipts_and_preserve_effects) {
    Fixture f;
    auto io = std::make_shared<FaultIo>();
    auto factory = storage::detail::makeLocalMaintenanceFileStore(io);
    BOOST_REQUIRE(factory);
    auto files = std::move(factory).value();
    auto upload = id<UploadId>(8);
    f.write(f.root / ".anof/uploads" / (upload.toString() + ".part"), "tail");
    auto observed = files->observe(f.call, *f.lease, upload, InspectionMode::Quick);
    BOOST_REQUIRE(observed && observed.value());
    TruncateUnconfirmedTail tail{ upload, {}, UploadOffset::from(2).value(), observed.value()->evidence };
    io->point = FaultIo::Point::Truncate;
    auto failed = files->truncateTail(f.call, *f.lease, tail);
    BOOST_REQUIRE(!failed);
    BOOST_CHECK(failed.error().outcome == MaintenanceOutcome::Unknown);
    BOOST_CHECK_EQUAL(std::filesystem::file_size(f.root / ".anof/uploads" / (upload.toString() + ".part")), 4);
    io->point = FaultIo::Point::Sync;
    failed = files->truncateTail(f.call, *f.lease, tail);
    BOOST_REQUIRE(!failed);
    BOOST_CHECK(failed.error().code == MaintenanceError::PublicationOutcomeUnknown);
    BOOST_CHECK_EQUAL(std::filesystem::file_size(f.root / ".anof/uploads" / (upload.toString() + ".part")), 2);
    MaintenanceAction action{ id<MaintenanceActionId>(14), tail, digest() };
    MaintenanceEvidenceBinding binding{ bootstrapSeed().identity.instanceId, id<MaintenanceRunId>(10), id<MaintenancePlanId>(11), digest(), action.id, {} };
    BOOST_CHECK(!files->verifyAction(f.call, *f.lease, binding, action));
    io->point = FaultIo::Point::None;
    BOOST_REQUIRE(files->verifyAction(f.call, *f.lease, binding, action));
    auto blob = id<BlobId>(8);
    f.write(f.root / ".anof/objects/12" / blob.toString(), "object");
    io->point = FaultIo::Point::Read;
    BOOST_CHECK(!files->observe(f.call, *f.lease, blob, InspectionMode::Full));
    auto destination = f.access->acquireDestination(f.call, f.root, f.base / "failed-config");
    BOOST_REQUIRE(destination);
    io->point = FaultIo::Point::Write;
    io->error = ENOSPC;
    InstanceConfiguration config;
    config.developmentHttp = true;
    BOOST_CHECK(!files->writeConfiguration(f.call, *destination.value(), config));
    BOOST_CHECK(!std::filesystem::exists(f.base / "failed-config"));
    BOOST_CHECK(std::filesystem::exists(destination.value()->staging()));
}
BOOST_AUTO_TEST_CASE(unsafe_entries_and_destination_replacement_are_rejected) {
    Fixture f;
    auto blob = id<BlobId>(8);
    auto path = f.root / ".anof/objects/12" / blob.toString();
    std::filesystem::create_symlink(f.root / ".anof/config.toml", path);
    BOOST_CHECK(!f.files->observe(f.call, *f.lease, blob, InspectionMode::Full));
    auto inventory = f.files->inventory(f.call, *f.lease, InspectionMode::Quick);
    BOOST_REQUIRE(inventory);
    auto page = inventory.value()->next(f.call, PageSize::from(1).value());
    BOOST_REQUIRE(page);
    BOOST_REQUIRE_EQUAL(page.value().size(), 1);
    BOOST_CHECK(std::holds_alternative<MaintenanceFinding>(page.value()[0]));
    std::filesystem::remove(path);
    std::filesystem::create_hard_link(f.root / ".anof/config.toml", path);
    BOOST_CHECK(!f.files->observe(f.call, *f.lease, blob, InspectionMode::Full));
    std::filesystem::remove(path);
    BOOST_CHECK(!f.access->acquireDestination(f.call, f.root, f.root / "backup"));
    BOOST_CHECK(!f.access->acquireDestination(f.call, f.root, f.base));
    auto destination = f.access->acquireDestination(f.call, f.root, f.base / "target");
    BOOST_REQUIRE(destination);
    auto staging = destination.value()->staging();
    std::filesystem::rename(staging, staging.string() + "-held");
    BOOST_REQUIRE(::mkdir(staging.c_str(), 0700) == 0);
    InstanceConfiguration config;
    config.developmentHttp = true;
    BOOST_CHECK(!f.files->writeConfiguration(f.call, *destination.value(), config));
}

BOOST_AUTO_TEST_CASE(shared_and_trash_backup_preserves_source_credentials_and_restores_new_configuration) {
    Fixture f;
    auto seed = bootstrapSeed();
    const auto blob = id<BlobId>(8);
    f.write(f.root / ".anof/objects/12" / blob.toString(), "test");
    auto file = f.files->observe(f.call, *f.lease, blob, InspectionMode::Full);
    BOOST_REQUIRE(file && file.value());
    Blob content{ blob, FileLength::from(4).value(), *file.value()->sha256 };
    {
        auto db = f.db();
        const auto owner = seed.adminUserId.toString(), space = seed.privateSpaceId.toString(), parent = seed.privateRootId.toString(), trash = id<TrashId>(9).toString();
        BOOST_REQUIRE(db->execute("BEGIN;INSERT INTO blobs(id,size,sha256,state,created_at,updated_at,revision,unreferenced_at,charged_bytes) VALUES('" + blob.toString() + "',4,x'" + content.sha256.toString() + "',0,123,123,1,NULL,4);"
                                                                                                                                                                                                                   "INSERT INTO nodes(id,space_id,parent_id,name,kind,created_by,blob_id,revision,trash_entry_id,trashed,created_at,updated_at) VALUES('" +
                                  id<NodeId>(10).toString() + "','" + space + "','" + parent + "','active',1,'" + owner + "','" + blob.toString() + "',1,NULL,0,123,123),('" + id<NodeId>(11).toString() + "','" + space + "',NULL,'trashed',1,'" + owner + "','" + blob.toString() + "',1,'" + trash + "',1,123,123);"
                                                                                                                                                                                                                                                                                                        "INSERT INTO trash_entries VALUES('" +
                                  trash + "','" + space + "','" + id<NodeId>(11).toString() + "','" + parent + "','trashed',1,'" + owner + "',123,2592000000123,1,1,4,'" + owner + "');"
                                                                                                                                                                                   "INSERT INTO sessions VALUES(zeroblob(32),'" +
                                  owner + "',0,999999999,0,123,'cookie');"
                                          "INSERT INTO uploads(id,user_id,client_upload_id,space_id,original_parent_id,original_name,length,parent_id,name,durable_offset,state,generation,staging_location,created_at,last_progress_at,expires_at,cancel_requested,cleanup_pending) VALUES('" +
                                  id<UploadId>(15).toString() + "','" + owner + "','" + id<OperationId>(15).toString() + "','" + space + "','" + parent + "','pending',4,'" + parent + "','pending',0,0,1,0,123,123,999999999,0,0);COMMIT"));
    }
    auto destination = f.access->acquireDestination(f.call, f.root, f.base / "shared-backup");
    BOOST_REQUIRE(destination);
    auto snapshot = makeSqliteMetadataSnapshot(bootstrapBudget);
    BOOST_REQUIRE(snapshot);
    BOOST_REQUIRE(snapshot.value()->exportConsistent(f.call, *f.lease, *destination.value()));
    auto excluded = snapshot.value()->normalizeBackup(f.call, *destination.value());
    BOOST_REQUIRE(excluded);
    auto database = snapshot.value()->verifyBackup(f.call, *destination.value());
    BOOST_REQUIRE(database);
    BOOST_REQUIRE(f.files->copyObject(f.call, *f.lease, *destination.value(), content, file.value()->evidence));
    BOOST_REQUIRE(f.files->appendObjectIndex(f.call, *destination.value(), { content }));
    auto index = f.files->finishObjectIndex(f.call, *destination.value());
    BOOST_REQUIRE(index);
    BOOST_CHECK_EQUAL(index.value().count.value(), 1);
    InstanceConfiguration config;
    config.developmentHttp = true;
    auto configuration = f.files->writeConfiguration(f.call, *destination.value(), config);
    BOOST_REQUIRE(configuration);
    BackupManifest manifest{ 1, "sqlite", "test", 5, 5, id<BackupId>(15), seed.identity.instanceId, now(), database.value(), configuration.value(), index.value(), excluded.value() };
    BOOST_REQUIRE(f.files->sealBackup(f.call, *destination.value(), manifest));
    BOOST_REQUIRE(f.files->publish(f.call, *destination.value()));
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM sessions"), 1);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM uploads"), 1);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM nodes WHERE blob_id IS NOT NULL"), 2);
    auto backup = f.files->openBackup(f.call, f.base / "shared-backup");
    BOOST_REQUIRE(backup);
    checkObjectsSyncFailure(f, *snapshot.value(), *backup.value());
    checkBackupContext(f, f.base / "shared-backup");
    auto page = backup.value()->objects(f.call, PageSize::from(1).value(), {});
    BOOST_REQUIRE(page);
    BOOST_REQUIRE_EQUAL(page.value().items.size(), 1);
    auto restored = f.access->acquireDestination(f.call, f.root, f.base / "restored");
    BOOST_REQUIRE(restored);
    BOOST_REQUIRE(f.files->copyBackup(f.call, *backup.value(), *restored.value()));
    BOOST_CHECK(!std::filesystem::exists(restored.value()->staging() / "config.toml"));
    BOOST_REQUIRE(f.files->writeConfiguration(f.call, *restored.value(), config));
    BOOST_REQUIRE(snapshot.value()->prepareRestore(f.call, *restored.value(), seed.identity.instanceId));
    BOOST_REQUIRE(f.files->sealRestoredInstance(f.call, *restored.value(), seed.identity.instanceId));
    BOOST_REQUIRE(f.files->publish(f.call, *restored.value()));
    auto db = openSqliteDatabase(f.base / "restored/.anof/metadata.sqlite3");
    BOOST_REQUIRE(db);
    auto q = SqliteStatement::prepare(*db.value(), "SELECT (SELECT count(*) FROM sessions),(SELECT count(*) FROM uploads),(SELECT count(*) FROM blobs),(SELECT count(*) FROM nodes WHERE trashed=1),(SELECT retained_bytes FROM storage_accounting),(SELECT used_bytes FROM space_quotas WHERE space_id='" + seed.privateSpaceId.toString() + "')");
    BOOST_REQUIRE(q);
    BOOST_REQUIRE(q.value()->step().value());
    for (int col = 0; col < 6; ++col) BOOST_CHECK_EQUAL(q.value()->integer(col).value(), (std::array<int, 6>{ 0, 0, 1, 1, 4, 8 })[col]);
}

BOOST_AUTO_TEST_CASE(corrupt_rows_and_format_are_incomplete_findings_not_healthy_results) {
    Fixture f;
    {
        auto db = f.db();
        BOOST_REQUIRE(db->execute("PRAGMA ignore_check_constraints=ON;UPDATE storage_accounting SET retained_bytes=-1"));
    }
    auto capture = f.access->captureInspection(f.call, *f.lease, f.work);
    BOOST_REQUIRE(capture);
    auto inspection = makeSqliteMetadataInspection();
    auto reader = inspection.value()->open(f.call, *capture.value());
    BOOST_REQUIRE(reader);
    auto summary = reader.value()->summary(f.call);
    BOOST_REQUIRE(summary);
    BOOST_CHECK(summary.value().errors.value() > 0);
    auto findings = reader.value()->findings(f.call, PageSize::from(1).value(), {});
    BOOST_REQUIRE(findings);
    BOOST_CHECK_EQUAL(findings.value().items.size(), 1);
    BOOST_CHECK(!reader.value()->accounting(f.call));
    reader.value().reset();
    capture.value().reset();
    f.write(f.root / ".anof/instance.json", "{}");
    auto second = f.access->captureInspection(f.call, *f.lease, f.work);
    BOOST_REQUIRE(second);
    reader = inspection.value()->open(f.call, *second.value());
    BOOST_REQUIRE(reader);
    summary = reader.value()->summary(f.call);
    BOOST_REQUIRE(summary);
    BOOST_CHECK(!summary.value().complete);
    auto identity = reader.value()->identity(f.call);
    BOOST_REQUIRE(identity);
    BOOST_CHECK(!identity.value().formatVersion);
}

BOOST_AUTO_TEST_CASE(uncertain_repair_commit_requires_fresh_connection_readback) {
    SqliteFaultVfs fault;
    Fixture f;
    auto repair = makeSqliteMetadataRepair();
    auto session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    {
        auto blocker = f.db();
        BOOST_REQUIRE(blocker->beginWrite());
        auto busy = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(!busy);
        BOOST_CHECK(busy.error().code == MaintenanceError::Busy);
        BOOST_REQUIRE(blocker->rollback());
    }
    MaintenanceRun run{ id<MaintenanceRunId>(10), id<MaintenancePlanId>(11), bootstrapSeed().identity.instanceId, digest(), now(), now(), false, {} };
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->insertRun(f.call, run));
        BOOST_REQUIRE(tx.value()->appendAudit(f.call, { run.id, f.call.operationId, MaintenanceAuditKind::PlanStarted, MaintenanceOutcome::Confirmed, {}, {}, {}, now() }));
        fault.arm(SqliteFaultVfs::Failure::WalSync);
        auto committed = tx.value()->commit(f.call);
        BOOST_REQUIRE(fault.triggered());
        BOOST_REQUIRE(!committed);
        BOOST_CHECK(committed.error().code == MaintenanceError::CommitOutcomeUnknown);
        BOOST_CHECK(committed.error().outcome == MaintenanceOutcome::Unknown);
    }
    session.value().reset();
    session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    auto result = session.value()->findRun(f.call, run.plan);
    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM maintenance_audit_events"), result.value() ? 1 : 0);
}

BOOST_AUTO_TEST_CASE(publication_unknown_retains_target_and_raw_preservation_is_distinct) {
    Fixture f;
    auto destination = f.access->acquireDestination(f.call, f.root, f.base / "backup-unknown");
    BOOST_REQUIRE(destination);
    auto snapshot = makeSqliteMetadataSnapshot(bootstrapBudget);
    BOOST_REQUIRE(snapshot.value()->exportConsistent(f.call, *f.lease, *destination.value()));
    auto exclusions = snapshot.value()->normalizeBackup(f.call, *destination.value());
    BOOST_REQUIRE(exclusions);
    auto database = snapshot.value()->verifyBackup(f.call, *destination.value());
    BOOST_REQUIRE(database);
    InstanceConfiguration config;
    config.developmentHttp = true;
    auto configuration = f.files->writeConfiguration(f.call, *destination.value(), config);
    BOOST_REQUIRE(configuration);
    auto index = f.files->finishObjectIndex(f.call, *destination.value());
    BOOST_REQUIRE(index);
    BackupManifest manifest{ 1, "sqlite", "test", 5, 5, id<BackupId>(15), bootstrapSeed().identity.instanceId, now(), database.value(), configuration.value(), index.value(), exclusions.value() };
    BOOST_REQUIRE(f.files->sealBackup(f.call, *destination.value(), manifest));
    auto io = std::make_shared<FaultIo>();
    auto files = storage::detail::makeLocalMaintenanceFileStore(io);
    io->point = FaultIo::Point::Rename;
    BOOST_CHECK(!files.value()->publish(f.call, *destination.value()));
    BOOST_CHECK(std::filesystem::exists(destination.value()->staging()));
    BOOST_CHECK(!std::filesystem::exists(destination.value()->target()));
    io->point = FaultIo::Point::None;
    io->error = ENOSPC;
    auto published = files.value()->publish(f.call, *destination.value());
    BOOST_REQUIRE(!published);
    BOOST_CHECK(published.error().outcome == MaintenanceOutcome::Unknown);
    BOOST_CHECK(published.error().artifact == destination.value()->target());
    BOOST_CHECK(std::filesystem::exists(destination.value()->target()));
    BOOST_CHECK(!files.value()->publish(f.call, *destination.value()));
    BOOST_REQUIRE(f.files->openBackup(f.call, destination.value()->target()));
    auto capture = f.access->captureInspection(f.call, *f.lease, f.work);
    BOOST_REQUIRE(capture);
    auto raw = f.access->acquireDestination(f.call, f.root, f.base / "raw-original");
    BOOST_REQUIRE(raw);
    BOOST_REQUIRE(f.files->preserveOriginal(f.call, *f.lease, *raw.value(), capture.value()->fingerprint()));
    BOOST_CHECK(std::filesystem::exists(raw.value()->target() / "COMPLETE"));
    BOOST_CHECK(!f.files->openBackup(f.call, raw.value()->target()));
}

BOOST_AUTO_TEST_CASE(recompute_requires_authoritative_proof_and_cannot_skip_action_state) {
    Fixture f;
    {
        auto db = f.db();
        BOOST_REQUIRE(db->execute("UPDATE storage_accounting SET retained_bytes=99"));
    }
    auto capture = f.access->captureInspection(f.call, *f.lease, f.work);
    BOOST_REQUIRE(capture);
    auto inspection = makeSqliteMetadataInspection();
    auto reader = inspection.value()->open(f.call, *capture.value());
    BOOST_REQUIRE(reader);
    auto facts = reader.value()->accounting(f.call);
    BOOST_REQUIRE(facts);
    auto repair = makeSqliteMetadataRepair();
    auto session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    MaintenanceRun run{ id<MaintenanceRunId>(10), id<MaintenancePlanId>(11), bootstrapSeed().identity.instanceId, digest(), now(), now(), false, {} };
    RecomputeAccounting payload{ facts.value().provenLedger.value() };
    MaintenanceAction action{ id<MaintenanceActionId>(14), payload, digest() };
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->insertRun(f.call, run));
        BOOST_REQUIRE(tx.value()->prepareAction(f.call, run.id, { {}, action, MaintenanceActionState::Prepared, {}, now(), now(), {} }));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    MaintenanceActionEvidence evidence{ { run.instance, run.id, run.plan, run.planDigest, action.id, {} }, MaintenanceEvidenceQuality::Durable, {}, action.expectedPostcondition };
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_CHECK(!tx.value()->advanceAction(f.call, run.id, action.id, {}, MaintenanceActionState::Applied, evidence, now()));
        BOOST_CHECK(!tx.value()->commit(f.call));
    }
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->recomputeAccounting(f.call, payload));
        BOOST_CHECK(!tx.value()->advanceAction(f.call, run.id, action.id, {}, MaintenanceActionState::Verified, evidence, now()));
        BOOST_CHECK(!tx.value()->commit(f.call));
    }
    BOOST_CHECK_EQUAL(f.scalar("SELECT retained_bytes FROM storage_accounting"), 99);
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->recomputeAccounting(f.call, payload));
        BOOST_REQUIRE(tx.value()->advanceAction(f.call, run.id, action.id, {}, MaintenanceActionState::Applied, evidence, now()));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    BOOST_CHECK_EQUAL(f.scalar("SELECT retained_bytes FROM storage_accounting"), 0);
}

BOOST_AUTO_TEST_CASE(restored_blob_requires_full_hash_and_keeps_original_identity) {
    Fixture f;
    const auto blob = id<BlobId>(8);
    f.write(f.root / ".anof/objects/12" / blob.toString(), "test");
    auto observed = f.files->observe(f.call, *f.lease, blob, InspectionMode::Full);
    BOOST_REQUIRE(observed && observed.value());
    {
        auto db = f.db();
        BOOST_REQUIRE(db->execute("INSERT INTO blobs(id,size,sha256,state,created_at,updated_at,revision,charged_bytes,unavailable_reason) VALUES('" + blob.toString() + "',4,x'" + observed.value()->sha256->toString() + "',2,123,123,2,4,0);UPDATE storage_accounting SET retained_bytes=4"));
    }
    auto repair = makeSqliteMetadataRepair();
    auto session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    MaintenanceRun run{ id<MaintenanceRunId>(10), id<MaintenancePlanId>(11), bootstrapSeed().identity.instanceId, digest(), now(), now(), false, {} };
    VerifyRestoredBlob payload{ blob, Revision::from(2).value(), observed.value()->evidence, *observed.value()->sha256 };
    MaintenanceAction action{ id<MaintenanceActionId>(14), payload, digest() };
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->insertRun(f.call, run));
        BOOST_REQUIRE(tx.value()->prepareAction(f.call, run.id, { {}, action, MaintenanceActionState::Prepared, {}, now(), now(), {} }));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    MaintenanceEvidenceBinding binding{ run.instance, run.id, run.plan, run.planDigest, action.id, {} };
    auto receipt = f.files->verifyAction(f.call, *f.lease, binding, action);
    BOOST_REQUIRE(receipt);
    {
        auto bad = receipt.value();
        bad.files[0].sha256 = digest();
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_CHECK(!tx.value()->restoreAvailable(f.call, payload, bad, now()));
        BOOST_CHECK(!tx.value()->commit(f.call));
    }
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->restoreAvailable(f.call, payload, receipt.value(), now()));
        BOOST_REQUIRE(tx.value()->advanceAction(f.call, run.id, action.id, {}, MaintenanceActionState::Applied, receipt.value(), now()));
        BOOST_REQUIRE(tx.value()->commit(f.call));
    }
    BOOST_CHECK_EQUAL(f.scalar("SELECT state FROM blobs"), 0);
    BOOST_CHECK_EQUAL(f.scalar("SELECT revision FROM blobs"), 3);
    BOOST_CHECK_EQUAL(f.scalar("SELECT charged_bytes FROM blobs"), 4);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM blobs WHERE unavailable_reason IS NULL AND unreferenced_at IS NOT NULL"), 1);
    {
        auto db = f.db();
        BOOST_REQUIRE(db->execute("UPDATE blobs SET revision=revision+1"));
    }
    binding.actionRevision = Revision::from(2).value();
    receipt = f.files->verifyAction(f.call, *f.lease, binding, action);
    BOOST_REQUIRE(receipt);
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_CHECK(!tx.value()->advanceAction(f.call, run.id, action.id, binding.actionRevision, MaintenanceActionState::Verified, receipt.value(), now()));
        BOOST_CHECK(!tx.value()->commit(f.call));
    }
    BOOST_CHECK_EQUAL(f.scalar("SELECT state FROM maintenance_actions"), 1);
}

BOOST_AUTO_TEST_CASE(inspection_identity_is_checked_before_any_getter_and_report_is_stable) {
    for (int damage = 0; damage < 6; ++damage) {
        Fixture f;
        auto capture = f.access->captureInspection(f.call, *f.lease, f.work);
        BOOST_REQUIRE(capture);
        const auto format = capture.value()->root() / "instance.json";
        if (damage == 1) f.write(format, system::detail::formatText({ id<InstanceId>(1), id<OperationId>(1) }));
        if (damage == 2) std::filesystem::remove(format);
        if (damage == 3) f.write(format, "{}");
        if (damage >= 4) {
            auto db = openSqliteDatabase(capture.value()->root() / "metadata.sqlite3");
            BOOST_REQUIRE(db);
            BOOST_REQUIRE(db.value()->execute("PRAGMA foreign_keys=OFF; DELETE FROM instance_settings"));
            if (damage == 5) BOOST_REQUIRE(db.value()->execute("PRAGMA ignore_check_constraints=ON; INSERT INTO instance_settings VALUES(1,'bad-id','bad-operation',123,1,1024,10,1024,10,1)"));
        }
        auto inspection = makeSqliteMetadataInspection();
        BOOST_REQUIRE(inspection);
        for (int order = 0; order < 3; ++order) {
            auto opened = inspection.value()->open(f.call, *capture.value());
            BOOST_REQUIRE(opened);
            auto& reader = *opened.value();
            if (order == 1) BOOST_REQUIRE(reader.identity(f.call));
            if (order == 2) {
                auto initialAccounting = reader.accounting(f.call);
                if (initialAccounting) BOOST_CHECK_EQUAL(initialAccounting.value().complete, damage == 0);
            }
            auto first = reader.summary(f.call);
            BOOST_REQUIRE(first);
            BOOST_CHECK_EQUAL(first.value().complete, damage == 0);
            BOOST_CHECK((first.value().status() == MaintenanceReportStatus::Healthy) == (damage == 0));
            auto findings = reader.findings(f.call, PageSize::from(200).value(), {});
            BOOST_REQUIRE(findings);
            auto account = reader.accounting(f.call);
            if (account)
                BOOST_CHECK_EQUAL(account.value().complete, damage == 0);
            else
                BOOST_CHECK(damage != 0);
            for (int repeat = 0; repeat < 3; ++repeat) {
                auto identity = reader.identity(f.call);
                BOOST_REQUIRE(identity);
                auto summary = reader.summary(f.call);
                BOOST_REQUIRE(summary);
                auto again = reader.findings(f.call, PageSize::from(200).value(), {});
                BOOST_REQUIRE(again);
                BOOST_CHECK_EQUAL(summary.value().complete, first.value().complete);
                BOOST_CHECK_EQUAL(summary.value().errors.value(), first.value().errors.value());
                BOOST_CHECK_EQUAL(again.value().items.size(), findings.value().items.size());
                if (damage == 0 || damage == 1) BOOST_CHECK(identity.value().instance == bootstrapSeed().identity.instanceId);
            }
        }
    }
}

namespace {
// 返回后 creation 和 flag 均已销毁，不能靠 Fixture 的长生命周期掩盖借用。
template <class F>
auto withTemporaryContext(OperationId operation, F&& make) {
    auto flag = std::make_unique<std::atomic<bool>>(false);
    MaintenanceContext creation{ operation, *flag };
    return make(creation);
}
template <class T>
void checkEntry(const MaintenanceResult<T>& result, const MaintenanceContext& call,
                MaintenanceStage stage, MaintenanceError code) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
    BOOST_CHECK(result.error().operationId == call.operationId);
    BOOST_CHECK(result.error().stage == stage);
    BOOST_CHECK(result.error().outcome == MaintenanceOutcome::NotStarted);
    BOOST_CHECK(result.error().reportStatus == MaintenanceReportStatus::Incomplete);
}
MaintenanceRun testRun(unsigned n = 10) {
    return { id<MaintenanceRunId>(n), id<MaintenancePlanId>(n), bootstrapSeed().identity.instanceId, digest(), now(), now(), false, {} };
}
// 只在本测试的线程/作用域注册，抓取真实适配器连接，不更换被测实现。
struct SqlProbe {
    static SqlProbe* current;
    sqlite3* db = nullptr;
    std::atomic<bool>* flag = nullptr;
    std::string match;
    bool triggered = false;
    static int opened(sqlite3* db, char**, const sqlite3_api_routines*) {
        const char* filename = sqlite3_db_filename(db, "main");
        if (filename && std::string(filename).find("metadata.sqlite3") != std::string::npos) current->db = db;
        return SQLITE_OK;
    }
    static int trace(unsigned, void* p, void*, void* sql) {
        auto& self = *static_cast<SqlProbe*>(p);
        if (self.flag && std::string(static_cast<const char*>(sql)).find(self.match) != std::string::npos) {
            self.flag->store(true);
            self.triggered = true;
            self.flag = nullptr;
        }
        return 0;
    }
    SqlProbe() {
        current = this;
        BOOST_REQUIRE_EQUAL(sqlite3_auto_extension(reinterpret_cast<void (*)()>(opened)), SQLITE_OK);
    }
    ~SqlProbe() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(opened));
        current = nullptr;
    }
    void arm(std::atomic<bool>& cancelled, const std::string& sql) {
        flag = &cancelled;
        match = sql;
        triggered = false;
        BOOST_REQUIRE_EQUAL(sqlite3_trace_v2(db, SQLITE_TRACE_STMT, trace, this), SQLITE_OK);
    }
    void clearTrace() {
        sqlite3_trace_v2(db, 0, nullptr, nullptr);
        flag = nullptr;
    }
    void proveProgressRemoved() {
        // 超过 1000 条 VM 指令；前次取消仍为 true，遗留 handler 会中断此查询。
        BOOST_CHECK_EQUAL(sqlite3_exec(db, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<2048) SELECT sum(x) FROM n", nullptr, nullptr, nullptr), SQLITE_OK);
    }
};
SqlProbe* SqlProbe::current = nullptr;
struct CancelIo : system::maintenance::Io {
    std::atomic<bool>* flag = nullptr;
    bool onRead = false, onRename = false, failSync = false, triggered = false;
    ssize_t read(int fd, void* data, std::size_t size) override {
        auto result = Io::read(fd, data, size);
        if (onRead && flag) {
            flag->store(true);
            triggered = true;
            onRead = false;
        }
        return result;
    }
    int rename(int from, const char* name, int to, const char* dest) override {
        auto result = Io::rename(from, name, to, dest);
        if (!result && onRename && flag) {
            flag->store(true);
            triggered = true;
        }
        return result;
    }
    int sync(int fd) override {
        if (triggered && failSync) {
            errno = EIO;
            return -1;
        }
        return Io::sync(fd);
    }
};
}  // namespace

BOOST_AUTO_TEST_CASE(returned_reader_and_session_use_only_each_calls_context) {
    Fixture f;
    auto capture = f.access->captureInspection(f.call, *f.lease, f.work);
    BOOST_REQUIRE(capture);
    auto inspection = makeSqliteMetadataInspection();
    SqlProbe probe;
    auto reader = withTemporaryContext(f.call.operationId, [&](const auto& c) { return inspection.value()->open(c, *capture.value()); });
    BOOST_REQUIRE(reader);
    std::atomic<bool> cancelled{ true };
    MaintenanceContext stop{ f.call.operationId, cancelled }, wrong{ id<OperationId>(8), cancelled };
    const auto size = PageSize::from(1).value();
    const auto initial = reader.value()->summary(f.call).value();
    for (const auto& call : { wrong, stop }) {
        auto error = call.operationId == f.call.operationId ? MaintenanceError::Cancelled : MaintenanceError::InvalidArgument;
        checkEntry(reader.value()->identity(call), call, MaintenanceStage::Inspect, error);
        checkEntry(reader.value()->summary(call), call, MaintenanceStage::Inspect, error);
        checkEntry(reader.value()->findings(call, size, {}), call, MaintenanceStage::Inspect, error);
        checkEntry(reader.value()->objects(call, size, {}), call, MaintenanceStage::Inspect, error);
        checkEntry(reader.value()->uploads(call, size, {}), call, MaintenanceStage::Inspect, error);
        checkEntry(reader.value()->quarantine(call, size, {}), call, MaintenanceStage::Inspect, error);
        checkEntry(reader.value()->runs(call, size, {}), call, MaintenanceStage::Inspect, error);
        checkEntry(reader.value()->accounting(call), call, MaintenanceStage::Inspect, error);
    }
    auto summary = reader.value()->summary(f.call);
    BOOST_REQUIRE(summary);
    BOOST_CHECK_EQUAL(summary.value().errors.value(), initial.errors.value());
    BOOST_CHECK_EQUAL(summary.value().complete, initial.complete);
    BOOST_REQUIRE(reader.value()->accounting(f.call));
    f.cancelled = true;
    probe.proveProgressRemoved();
    f.cancelled = false;
    reader.value().reset();
    auto repair = makeSqliteMetadataRepair();
    auto session = withTemporaryContext(f.call.operationId, [&](const auto& c) { return repair.value()->open(c, *f.lease); });
    BOOST_REQUIRE(session);
    for (const auto& call : { wrong, stop }) {
        auto error = call.operationId == f.call.operationId ? MaintenanceError::Cancelled : MaintenanceError::InvalidArgument;
        checkEntry(session.value()->findRun(call, testRun().plan), call, MaintenanceStage::Recover, error);
        checkEntry(session.value()->actions(call, testRun().id, size, {}), call, MaintenanceStage::Recover, error);
        checkEntry(session.value()->beginWrite(call), call, MaintenanceStage::Recover, error);
    }
    auto tx = withTemporaryContext(f.call.operationId, [&](const auto& c) { return session.value()->beginWrite(c); });
    BOOST_REQUIRE(tx);
    BOOST_REQUIRE(tx.value()->insertRun(f.call, testRun()));
    BOOST_REQUIRE(tx.value()->commit(f.call));
    tx.value().reset();
    BOOST_REQUIRE(session.value()->findRun(f.call, testRun().plan).value());
    f.cancelled = true;
    probe.proveProgressRemoved();
    f.cancelled = false;
}

BOOST_AUTO_TEST_CASE(repair_entry_failure_poisoning_and_destructor_rollback_ignore_cancelled_flags) {
    Fixture f;
    auto repair = makeSqliteMetadataRepair();
    auto session = withTemporaryContext(f.call.operationId, [&](const auto& c) { return repair.value()->open(c, *f.lease); });
    BOOST_REQUIRE(session);
    std::atomic<bool> cancelled{ true };
    MaintenanceContext stop{ f.call.operationId, cancelled }, wrong{ id<OperationId>(8), cancelled };
    for (const auto& call : { wrong, stop }) {
        for (int rejectCommit = 0; rejectCommit < 2; ++rejectCommit) {
            auto tx = withTemporaryContext(f.call.operationId, [&](const auto& c) { return session.value()->beginWrite(c); });
            BOOST_REQUIRE(tx);
            BOOST_REQUIRE(tx.value()->insertRun(f.call, testRun()));
            auto result = rejectCommit ? tx.value()->commit(call) : tx.value()->insertRun(call, testRun(11));
            checkEntry(result, call, MaintenanceStage::Recover, call.operationId == f.call.operationId ? MaintenanceError::Cancelled : MaintenanceError::InvalidArgument);
            BOOST_CHECK(!tx.value()->commit(f.call));
            tx.value().reset();
            BOOST_CHECK(!session.value()->findRun(f.call, testRun().plan).value());
        }
    }
    {
        auto tx = withTemporaryContext(f.call.operationId, [&](const auto& c) { return session.value()->beginWrite(c); });
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->insertRun(f.call, testRun()));
        // 无 commit，调用的上下文已销毁，析构仍独立回滚。
    }
    BOOST_CHECK(!session.value()->findRun(f.call, testRun().plan).value());
}

BOOST_AUTO_TEST_CASE(sql_cancellation_clears_callbacks_and_cannot_commit_partial_writes) {
    Fixture f;
    auto repair = makeSqliteMetadataRepair();
    SqlProbe probe;
    auto session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    BOOST_REQUIRE_EQUAL(sqlite3_exec(probe.db,
                                     "CREATE TEMP TRIGGER slow_insert BEFORE INSERT ON maintenance_runs BEGIN "
                                     "SELECT (WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<4096) SELECT sum(x) FROM n); END",
                                     nullptr, nullptr, nullptr),
                        SQLITE_OK);
    auto tx = session.value()->beginWrite(f.call);
    BOOST_REQUIRE(tx);
    BOOST_REQUIRE(tx.value()->insertRun(f.call, testRun()));
    std::atomic<bool> flag{ false };
    MaintenanceContext call{ f.call.operationId, flag };
    probe.arm(flag, "INSERT INTO maintenance_runs");
    auto result = tx.value()->insertRun(call, testRun(11));
    BOOST_REQUIRE(!result);
    BOOST_CHECK(probe.triggered);
    BOOST_CHECK(result.error().code == MaintenanceError::Cancelled);
    probe.clearTrace();
    probe.proveProgressRemoved();
    BOOST_CHECK(!tx.value()->commit(f.call));
    tx.value().reset();
    BOOST_CHECK(!session.value()->findRun(f.call, testRun().plan).value());
    BOOST_CHECK(!session.value()->findRun(f.call, testRun(11).plan).value());
}

BOOST_AUTO_TEST_CASE(commit_checks_cancellation_before_submission_but_keeps_late_success) {
    Fixture f;
    auto repair = makeSqliteMetadataRepair();
    SqlProbe probe;
    auto session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    for (bool afterStart : { false, true }) {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        auto run = testRun(afterStart ? 12 : 11);
        BOOST_REQUIRE(tx.value()->insertRun(f.call, run));
        std::atomic<bool> flag{ false };
        MaintenanceContext call{ f.call.operationId, flag };
        probe.arm(flag, afterStart ? "COMMIT" : "SELECT NOT EXISTS");
        auto result = tx.value()->commit(call);
        BOOST_CHECK(probe.triggered);
        if (afterStart)
            BOOST_REQUIRE(result);
        else {
            BOOST_REQUIRE(!result);
            BOOST_CHECK(result.error().code == MaintenanceError::Cancelled);
        }
        probe.clearTrace();
        probe.proveProgressRemoved();
        tx.value().reset();
        BOOST_CHECK_EQUAL(bool(session.value()->findRun(f.call, run.plan).value()), afterStart);
    }
}

BOOST_AUTO_TEST_CASE(inventory_lifetime_entry_rejection_and_interrupted_cursor_require_reopen) {
    Fixture f;
    f.write(f.root / ".anof/objects/12" / id<BlobId>(0).toString(), std::string(128 * 1024, 'x'));
    f.write(f.root / ".anof/objects/12" / id<BlobId>(1).toString(), "next");
    auto io = std::make_shared<CancelIo>();
    auto files = storage::detail::makeLocalMaintenanceFileStore(io);
    auto inventory = withTemporaryContext(f.call.operationId, [&](const auto& c) { return files.value()->inventory(c, *f.lease, InspectionMode::Full); });
    BOOST_REQUIRE(inventory);
    std::atomic<bool> flag{ true };
    MaintenanceContext stop{ f.call.operationId, flag }, wrong{ id<OperationId>(8), flag };
    auto size = PageSize::from(1).value();
    checkEntry(inventory.value()->next(wrong, size), wrong, MaintenanceStage::Inspect, MaintenanceError::InvalidArgument);
    checkEntry(inventory.value()->next(stop, size), stop, MaintenanceStage::Inspect, MaintenanceError::Cancelled);
    auto first = inventory.value()->next(f.call, size);
    BOOST_REQUIRE(first && first.value().size() == 1);
    checkEntry(inventory.value()->next(stop, size), stop, MaintenanceStage::Inspect, MaintenanceError::Cancelled);
    auto second = inventory.value()->next(f.call, size);
    BOOST_REQUIRE(second && second.value().size() == 1);
    BOOST_CHECK(std::get<MaintenanceFileObservation>(first.value()[0]).key != std::get<MaintenanceFileObservation>(second.value()[0]).key);
    inventory = files.value()->inventory(f.call, *f.lease, InspectionMode::Full);
    flag = false;
    io->flag = &flag;
    io->onRead = true;
    auto interrupted = inventory.value()->next(stop, size);
    BOOST_REQUIRE(!interrupted);
    BOOST_CHECK(io->triggered);
    BOOST_CHECK(interrupted.error().code == MaintenanceError::Cancelled);
    BOOST_CHECK(!inventory.value()->next(f.call, size));
    inventory.value().reset();
    inventory = files.value()->inventory(f.call, *f.lease, InspectionMode::Full);
    BOOST_REQUIRE(inventory.value()->next(f.call, size));
}

namespace {
void checkBackupContext(Fixture& f, const std::filesystem::path& path) {
    auto io = std::make_shared<CancelIo>();
    auto files = storage::detail::makeLocalMaintenanceFileStore(io);
    auto backup = withTemporaryContext(f.call.operationId, [&](const auto& call) { return files.value()->openBackup(call, path); });
    BOOST_REQUIRE(backup);
    std::atomic<bool> flag{ true };
    MaintenanceContext stop{ f.call.operationId, flag }, wrong{ id<OperationId>(8), flag };
    const auto size = PageSize::from(1).value();
    checkEntry(backup.value()->objects(wrong, size, {}), wrong, MaintenanceStage::Verify, MaintenanceError::InvalidArgument);
    checkEntry(backup.value()->objects(stop, size, {}), stop, MaintenanceStage::Verify, MaintenanceError::Cancelled);
    auto page = backup.value()->objects(f.call, size, {});
    BOOST_REQUIRE(page);
    BOOST_CHECK_EQUAL(page.value().items.size(), backup.value()->manifest().objects.count.value());
    checkEntry(backup.value()->objects(stop, size, {}), stop, MaintenanceStage::Verify, MaintenanceError::Cancelled);
    auto repeated = backup.value()->objects(f.call, size, {});
    BOOST_REQUIRE(repeated);
    BOOST_CHECK_EQUAL(repeated.value().items.size(), page.value().items.size());
    flag = false;
    io->flag = &flag;
    io->onRead = true;
    auto interrupted = backup.value()->objects(stop, size, {});
    BOOST_REQUIRE(!interrupted);
    BOOST_CHECK(io->triggered);
    BOOST_CHECK(interrupted.error().code == MaintenanceError::Cancelled);
    // Backup 无持久游标，下一次按显式位置重新校验索引。
    BOOST_REQUIRE(backup.value()->objects(f.call, size, {}));
    flag = false;
    auto dest = f.access->acquireDestination(wrong, f.root, f.base / "context-copy");
    BOOST_REQUIRE(dest);
    auto rejected = files.value()->copyBackup(wrong, *backup.value(), *dest.value());
    checkEntry(rejected, wrong, MaintenanceStage::Verify, MaintenanceError::InvalidArgument);
    BOOST_CHECK(!std::filesystem::exists(dest.value()->staging() / "metadata.sqlite3"));
    auto reopened = withTemporaryContext(wrong.operationId, [&](const auto& call) { return files.value()->openBackup(call, path); });
    BOOST_REQUIRE(reopened);
    io->triggered = false;
    io->onRead = true;
    auto partial = files.value()->copyBackup(wrong, *reopened.value(), *dest.value());
    BOOST_REQUIRE(!partial);
    BOOST_CHECK(io->triggered);
    BOOST_CHECK(partial.error().code == MaintenanceError::Cancelled);
    BOOST_CHECK(partial.error().outcome == MaintenanceOutcome::Unknown);
    BOOST_CHECK(std::filesystem::exists(dest.value()->staging() / "metadata.sqlite3"));
    BOOST_CHECK(!files.value()->publish(f.call, *dest.value()));
}
}  // namespace

BOOST_AUTO_TEST_CASE(reader_interrupted_scan_requires_reopen_without_healthy_summary) {
    Fixture f;
    {
        auto db = f.db();
        BOOST_REQUIRE(db->execute(
            "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<256) "
            "INSERT INTO blobs(id,size,sha256,state,created_at,updated_at,revision,unreferenced_at,charged_bytes) "
            "SELECT printf('12000000-0000-0000-0000-%012d',x),0,zeroblob(32),0,123,123,1,123,0 FROM n"));
    }
    auto capture = f.access->captureInspection(f.call, *f.lease, f.work);
    BOOST_REQUIRE(capture);
    auto inspection = makeSqliteMetadataInspection();
    SqlProbe probe;
    auto reader = inspection.value()->open(f.call, *capture.value());
    BOOST_REQUIRE(reader);
    std::atomic<bool> flag{ false };
    MaintenanceContext call{ f.call.operationId, flag };
    probe.arm(flag, "SELECT id FROM blobs");
    auto result = reader.value()->objects(call, PageSize::from(200).value(), {});
    BOOST_REQUIRE(!result);
    BOOST_CHECK(probe.triggered);
    BOOST_CHECK(result.error().code == MaintenanceError::Cancelled);
    probe.clearTrace();
    probe.proveProgressRemoved();
    BOOST_CHECK(!reader.value()->summary(f.call));
    reader.value().reset();
    reader = inspection.value()->open(f.call, *capture.value());
    BOOST_REQUIRE(reader);
    auto page = reader.value()->objects(f.call, PageSize::from(1).value(), {});
    BOOST_REQUIRE(page && page.value().items.size() == 1);
    BOOST_CHECK_EQUAL(page.value().items[0].record.content.id.toString(), "12000000-0000-0000-0000-000000000001");
    checkEntry(reader.value()->objects(call, PageSize::from(1).value(), page.value().next), call, MaintenanceStage::Inspect, MaintenanceError::Cancelled);
    auto second = reader.value()->objects(f.call, PageSize::from(1).value(), page.value().next);
    BOOST_REQUIRE(second && second.value().items.size() == 1);
    BOOST_CHECK_EQUAL(second.value().items[0].record.content.id.toString(), "12000000-0000-0000-0000-000000000002");
}

BOOST_AUTO_TEST_CASE(late_cancel_does_not_hide_unknown_commit_or_published_directory) {
    Fixture f;
    SqliteFaultVfs faults;
    auto repair = makeSqliteMetadataRepair();
    SqlProbe probe;
    auto session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    {
        auto tx = session.value()->beginWrite(f.call);
        BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->insertRun(f.call, testRun()));
        std::atomic<bool> flag{ false };
        MaintenanceContext call{ f.call.operationId, flag };
        probe.arm(flag, "COMMIT");
        faults.arm(SqliteFaultVfs::Failure::WalSync);
        auto committed = tx.value()->commit(call);
        BOOST_REQUIRE(!committed);
        BOOST_CHECK(probe.triggered);
        BOOST_CHECK(faults.triggered());
        BOOST_CHECK(committed.error().code == MaintenanceError::CommitOutcomeUnknown);
        BOOST_CHECK(committed.error().outcome == MaintenanceOutcome::Unknown);
        probe.clearTrace();
    }
    session.value().reset();
    session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    BOOST_REQUIRE(session.value()->findRun(f.call, testRun().plan));
    session.value().reset();
    auto capture = f.access->captureInspection(f.call, *f.lease, f.work);
    BOOST_REQUIRE(capture);
    for (bool failSync : { false, true }) {
        std::atomic<bool> flag{ false };
        MaintenanceContext call{ id<OperationId>(failSync ? 5 : 4), flag };
        auto dest = f.access->acquireDestination(call, f.root, f.base / (failSync ? "raw-unknown-cancel" : "raw-late-cancel"));
        BOOST_REQUIRE(dest);
        auto io = std::make_shared<CancelIo>();
        io->flag = &flag;
        io->onRename = true;
        io->failSync = failSync;
        auto files = storage::detail::makeLocalMaintenanceFileStore(io);
        auto result = files.value()->preserveOriginal(call, *f.lease, *dest.value(), capture.value()->fingerprint());
        BOOST_CHECK(io->triggered);
        BOOST_CHECK(std::filesystem::exists(dest.value()->target() / "COMPLETE"));
        if (failSync) {
            BOOST_REQUIRE(!result);
            BOOST_CHECK(result.error().code == MaintenanceError::PublicationOutcomeUnknown);
            BOOST_CHECK(result.error().outcome == MaintenanceOutcome::Unknown);
        } else
            BOOST_REQUIRE(result);
    }
}

BOOST_AUTO_TEST_CASE(every_repair_write_entry_rejects_before_effects_and_disallows_later_commit) {
    Fixture f;
    auto repair = makeSqliteMetadataRepair();
    auto session = repair.value()->open(f.call, *f.lease);
    BOOST_REQUIRE(session);
    const auto run = testRun();
    MaintenanceAction action{ id<MaintenanceActionId>(14), RecomputeAccounting{ digest() }, digest() };
    MaintenanceActionRecord record{ {}, action, MaintenanceActionState::Prepared, {}, now(), now(), {} };
    MaintenanceActionEvidence evidence{ { run.instance, run.id, run.plan, run.planDigest, action.id, {} }, MaintenanceEvidenceQuality::Durable, {}, digest() };
    QuarantineEntry entry{ id<QuarantineId>(13), id<BlobId>(8), {}, digest(), {}, QuarantineState::Prepared, run.plan, action.id, now(), now(), {} };
    using Invoke = std::function<MaintenanceResult<void>(MetadataRepairTransaction&, const MaintenanceContext&)>;
    const std::vector<Invoke> methods{
        [&](auto& tx, const auto& c) { return tx.insertRun(c, testRun(11)); },
        [&](auto& tx, const auto& c) { return tx.prepareAction(c, run.id, record); },
        [&](auto& tx, const auto& c) { return tx.advanceAction(c, run.id, action.id, {}, MaintenanceActionState::Applied, evidence, now()); },
        [&](auto& tx, const auto& c) { return tx.completeRun(c, run.id, {}, now()); },
        [&](auto& tx, const auto& c) { return tx.markUnavailable(c, { id<BlobId>(8), {}, { UnavailableReason::Missing, {}, {} } }, evidence, now()); },
        [&](auto& tx, const auto& c) { return tx.restoreAvailable(c, { id<BlobId>(8), {}, {}, digest() }, evidence, now()); },
        [&](auto& tx, const auto& c) { return tx.recomputeAccounting(c, { digest() }); },
        [&](auto& tx, const auto& c) { return tx.prepareQuarantine(c, entry); },
        [&](auto& tx, const auto& c) { return tx.holdQuarantine(c, entry.id, {}, evidence, now()); },
        [&](auto& tx, const auto& c) { return tx.appendAudit(c, { run.id, f.call.operationId, MaintenanceAuditKind::PlanStarted, MaintenanceOutcome::Confirmed, {}, {}, {}, now() }); },
        [&](auto& tx, const auto& c) { return tx.commit(c); }
    };
    std::atomic<bool> cancelled{ true };
    const MaintenanceContext stop{ f.call.operationId, cancelled }, wrong{ id<OperationId>(8), cancelled };
    for (const auto& invoke : methods) {
        for (const auto& call : { wrong, stop }) {
            auto tx = session.value()->beginWrite(f.call);
            BOOST_REQUIRE(tx);
            BOOST_REQUIRE(tx.value()->insertRun(f.call, run));
            checkEntry(invoke(*tx.value(), call), call, MaintenanceStage::Recover,
                       call.operationId == f.call.operationId ? MaintenanceError::Cancelled : MaintenanceError::InvalidArgument);
            BOOST_CHECK(!tx.value()->commit(f.call));
            tx.value().reset();
            auto absent = session.value()->findRun(f.call, run.plan);
            BOOST_REQUIRE(absent);
            BOOST_CHECK(!absent.value());
        }
    }
}

BOOST_AUTO_TEST_CASE(audit_budget_one_rotates_tables_instead_of_starving_newer_events) {
    Fixture f;
    auto db = f.db();
    for (int i = 0; i < 5; ++i) BOOST_REQUIRE(db->execute("INSERT INTO identity_audit_events(subject_user_id,actor_id,request_id,action,occurred_at) VALUES('" + bootstrapSeed().adminUserId.toString() + "','" + bootstrapSeed().adminUserId.toString() + "','" + id<RequestId>(1).toString() + "','session_created',1)"));
    BOOST_REQUIRE(db->execute("INSERT INTO transfer_audit_events(kind,request_id,actor_id,upload_id,occurred_at,failure) VALUES(6,'" + id<RequestId>(1).toString() + "','" + bootstrapSeed().adminUserId.toString() + "',NULL,999,NULL)"));
    auto retention = openSqliteAuditRetention(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(retention);
    for (int i = 0; i < 2; ++i) {
        auto result = retention.value()->eraseExpired({ now(), AuditBatchSize::from(1).value() });
        BOOST_REQUIRE(result);
        BOOST_CHECK_EQUAL(result.value().removed.value(), 1);
    }
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM transfer_audit_events"), 0);
    BOOST_CHECK_EQUAL(f.scalar("SELECT count(*) FROM identity_audit_events"), 5);
    auto rollback = retention.value()->eraseExpired({ UtcTime{ std::chrono::microseconds{ 0 } }, AuditBatchSize::from(10).value() });
    BOOST_REQUIRE(rollback);
    BOOST_CHECK_EQUAL(rollback.value().removed.value(), 0);
}
