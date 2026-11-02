#define BOOST_TEST_MODULE DriveMaintenanceIntegration
#include <anof/application/drive/MaintenanceService.h>
#include <anof/application/transfer/TransferRecovery.h>
#include <anof/infrastructure/storage/local/LocalTransferFileStore.h>
#include <anof/infrastructure/storage/local/internal/FileOperations.h>
#include <anof/runtime/InstanceServer.h>
#include <anof/runtime/MaintenanceExecutor.h>
#include <anof/runtime/UploadExecutor.h>
#include <muduox/net/EventLoop.h>
#include <sys/wait.h>
#include <tests/integration/DriveFixture.h>
#include <tests/integration/TransferFaultStore.h>
#include <tests/support/EventLoopWait.h>

#include <atomic>
#include <fstream>
#include <future>
using namespace anof;
using namespace anof::runtime;
using namespace anof::testing::drive;
namespace {
struct FixedClock : Clock {
    UtcTime value;
    explicit FixedClock(UtcTime v) : value(v) {}
    UtcTime now() const override { return value; }
};
template <class T>
T fileTake(FileResult<T> result) {
    BOOST_REQUIRE(std::holds_alternative<T>(result));
    return std::move(std::get<T>(result));
}
std::unique_ptr<TransferFileStore> files(const std::filesystem::path& root) { return fileTake(infrastructure::storage::openLocalTransferFileStore(root)); }
Upload publishFile(Fixture& f, const std::string& name = "content", std::int64_t length = 3) {
    auto upload = f.createUpload(f.personal->rootNodeId, name, length);
    auto fs = files(f.root);
    auto handle = fileTake(fs->create(upload.id));
    TransferBlock block;
    block.size = static_cast<std::size_t>(length);
    block.bytes[0] = 'a';
    block.bytes[1] = 'b';
    block.bytes[2] = 'c';
    if (length) fileTake(fs->write(*handle, {}, block));
    fileTake(fs->syncData(*handle));
    auto hash = fileTake(fs->beginHash());
    fileTake(fs->updateHash(*hash, block));
    auto digest = fileTake(fs->finishHash(std::move(hash)));
    handle.reset();
    if (length) {
        auto patch = take(f.uploads().preparePatch(*f.admin, { upload.id, {}, ByteCount::from(length).value() }));
        upload = take(f.uploads().commitPatch(*f.admin, patch));
    }
    upload = take(f.uploads().acceptComplete(*f.admin, { upload.id, {} }));
    upload = take(f.uploads().recordVerification(upload.id, upload.generation, digest, request()));
    upload = take(f.uploads().preparePublish(upload.id, upload.generation));
    fileTake(fs->publish(upload.id, *upload.pendingBlobId));
    return take(f.uploads().commitPublish(upload.id, upload.generation, request()));
}
TrashEntry trash(Fixture& f, NodeId id) {
    auto node = f.node(id);
    return take(f.service().execute(*f.admin, TrashNodeCommand{ id, NodeCondition{ id, node.metadata.revision } })).entry;
}
BlobRecord purge(Fixture& f, Upload u) {
    auto entry = trash(f, *u.resultNodeId);
    BOOST_REQUIRE(f.service().execute(*f.admin, PurgeTrashCommand{ entry.id, TrashCondition{ entry.id, entry.revision } }));
    auto read = take(f.store().beginDriveRead());
    auto b = take(read->blobLifecycle().record(*u.pendingBlobId));
    BOOST_REQUIRE(b);
    return *b;
}
void settled(Fixture& f, BlobId id) {
    f.reopen();
    auto read = take(f.store().beginDriveRead());
    BOOST_CHECK(!take(read->blobLifecycle().record(id)));
    BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 0);
    BOOST_CHECK_EQUAL(take(read->blobLifecycle().gcStatistics()).pendingObjects.value(), 0);
}
struct Faults {
    test::CommitFault commit;
    std::atomic<bool> failDisk{ false }, unknownDisk{ false }, armFinal{ false }, blockDisk{ false }, blockClock{ false }, entered{ false }, failReopen{ false };
    std::promise<void> released;
    std::shared_future<void> release = released.get_future().share();
    bool opened = false;
    void open() {
        if (!opened) {
            opened = true;
            released.set_value();
        }
    }
};
struct FileProxy : TransferFileStore {
    std::unique_ptr<TransferFileStore> inner;
    Faults& fault;
    FileProxy(std::unique_ptr<TransferFileStore> s, Faults& f) : inner(std::move(s)), fault(f) {}
    FileResult<OwnedTransferFile> create(UploadId id) override { return inner->create(id); }
    FileResult<OwnedTransferFile> open(const TransferFileKey& k, bool w) override { return inner->open(k, w); }
    FileResult<TransferFileStat> stat(TransferFileHandle& h) override { return inner->stat(h); }
    FileStatus write(TransferFileHandle& h, UploadOffset o, const TransferBlock& b) override { return inner->write(h, o, b); }
    FileStatus read(TransferFileHandle& h, UploadOffset o, std::size_t n, TransferBlock& b) override { return inner->read(h, o, n, b); }
    FileStatus syncData(TransferFileHandle& h) override { return inner->syncData(h); }
    FileStatus truncateAndSync(TransferFileHandle& h, UploadOffset o) override { return inner->truncateAndSync(h, o); }
    FileStatus publish(UploadId u, BlobId b) override { return inner->publish(u, b); }
    FileStatus syncPublication(UploadId u, BlobId b) override { return inner->syncPublication(u, b); }
    FileStatus removeAndSync(const TransferFileKey& k) override {
        if (fault.blockDisk.exchange(false)) {
            fault.entered = true;
            if (fault.release.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return TransferFileError::Transient;
        }
        if (fault.failDisk.exchange(false)) return TransferFileError::Transient;
        auto result = inner->removeAndSync(k);
        if (fault.armFinal.exchange(false)) fault.commit.armed = true;
        if (fault.unknownDisk.exchange(false)) return TransferFileError::UnknownEffect;
        return result;
    }
    FileResult<OwnedTransferHash> beginHash() override { return inner->beginHash(); }
    FileStatus updateHash(TransferHashState& h, const TransferBlock& b) override { return inner->updateHash(h, b); }
    FileResult<Sha256> finishHash(OwnedTransferHash h) override { return inner->finishHash(std::move(h)); }
    FileResult<Page<InventoryEntry, InventoryCursor>> inventory(InventoryArea a, PageSize p, std::optional<InventoryCursor> c) override { return inner->inventory(a, p, c); }
    FileResult<DiskCapacity> capacity() override { return inner->capacity(); }
};
struct Runtime {
    Fixture& f;
    Faults fault;
    muduox::net::EventLoop loop;
    MetadataExecutor metadata{ loop, { 4, 4 } };
    DiskExecutor disk{ loop };
    UploadExecutor uploads{ loop, metadata, disk, { ByteCount{}, SafetyPercent{} } };
    MaintenanceExecutor maintenance{ loop, metadata, disk, uploads.blobAccess() };
    Runtime(Fixture& fixture, UtcTime now) : f(fixture) {
        f.resources = {};
        bool m = false, d = false;
        BOOST_REQUIRE(metadata.start([this, now]() -> Result<MetadataWorkerResources> {
            auto r = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3"); if (!r) return r.error();
            struct WorkerClock : Clock {
                UtcTime value; Faults& faults;
                WorkerClock(UtcTime v, Faults& f) : value(v), faults(f) {}
                UtcTime now() const override {
                    if (faults.blockClock.exchange(false)) {
                        faults.entered = true;
                        if (faults.release.wait_for(std::chrono::seconds(5)) != std::future_status::ready) throw std::runtime_error("clock gate timeout");
                    }
                    return value;
                }
            };
            r.value().clock = std::make_unique<WorkerClock>(now, fault);
            auto* store = dynamic_cast<TransferMetadataStore*>(r.value().metadata.release());
            r.value().metadata = std::make_unique<test::TransferFaultStore>(std::unique_ptr<TransferMetadataStore>(store), fault.commit);
            auto reopen = r.value().reopenTransfer;
            r.value().reopenTransfer = [this, reopen]() -> Result<std::unique_ptr<TransferMetadataStore>> {
                if (fault.failReopen) return ErrorCode::Retryable;
                auto s = reopen(); if (!s) return s.error();
                return std::unique_ptr<TransferMetadataStore>(new test::TransferFaultStore(std::move(s).value(), fault.commit));
            };
            return std::move(r).value(); }, [&](auto r) {BOOST_REQUIRE(r);m=true; }));
        BOOST_REQUIRE(disk.start([this]() -> Result<std::unique_ptr<TransferFileStore>> {
            auto s = infrastructure::storage::openLocalTransferFileStore(f.root);
            if (!std::holds_alternative<std::unique_ptr<TransferFileStore>>(s)) return ErrorCode::IntegrityError;
            return std::unique_ptr<TransferFileStore>(new FileProxy(std::move(std::get<std::unique_ptr<TransferFileStore>>(s)), fault)); }, [&](auto r) {BOOST_REQUIRE(r);d=true; }, [] {}));
        wait([&] { return m && d; });
    }
    void wait(std::function<bool()> cb) { BOOST_REQUIRE(anof::test::waitFor(loop, cb)); }
    Result<void> recover() {
        std::optional<Result<void>> result;
        uploads.recover([&](auto r) { result = r; });
        wait([&] { return result.has_value(); });
        if (!*result) return *result;
        result.reset();
        maintenance.recover([&](auto r) { result = r; });
        wait([&] { return result.has_value(); });
        return *result;
    }
    void tick() {
        maintenance.tick();
        wait([&] { return !maintenance.busy(); });
    }
    ~Runtime() {
        fault.open();
        maintenance.requestStop([] {});
        uploads.requestStop([] {});
        if (!anof::test::waitFor(loop, [&] { return maintenance.stopped() && uploads.stopped(); })) std::terminate();
        disk.requestStop();
        if (!anof::test::waitFor(loop, [&] { return disk.snapshot().stopped; })) std::terminate();
        metadata.requestStop();
        if (!anof::test::waitFor(loop, [&] { return metadata.snapshot().state == MetadataExecutorState::Stopped; })) std::terminate();
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(retention_boundary_rechecks_and_keeps_physical_charge) {
    Fixture f;
    auto u = publishFile(f);
    auto entry = trash(f, *u.resultNodeId);
    FixedClock clock(entry.expiresAt - std::chrono::microseconds(1));
    MaintenanceService service(f.store(), clock);
    auto early = service.expire({ entry.id, entry.revision }, request());
    BOOST_REQUIRE(!early);
    BOOST_CHECK(early.error().code == ErrorCode::Conflict);
    clock.value = entry.expiresAt;
    BOOST_REQUIRE(service.expire({ entry.id, entry.revision }, request()));
    BOOST_REQUIRE(auditTransferMetadata(f.store(), clock.now()));
    auto read = take(f.store().beginDriveRead());
    BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 3);
    auto row = take(read->blobLifecycle().record(*u.pendingBlobId));
    BOOST_REQUIRE(row && row->unreferencedAt);
    BOOST_CHECK(*row->unreferencedAt == entry.expiresAt);
    BOOST_CHECK(take(read->uploads().find(u.id))->resultDeletedAt.has_value());
}
BOOST_AUTO_TEST_CASE(shared_and_trashed_completed_results_pass_recovery_without_resurrection) {
    Fixture f;
    auto u = publishFile(f);
    auto n = f.node(*u.resultNodeId);
    auto copy = take(f.service().execute(*f.admin, CopyFileCommand{ n.metadata.id, NodeCondition{ n.metadata.id, n.metadata.revision }, f.personal->id, f.personal->rootNodeId, "copy" }));
    auto entry = trash(f, n.metadata.id);
    BOOST_REQUIRE(auditTransferMetadata(f.store(), f.resources.clock->now()));
    BOOST_REQUIRE(f.service().execute(*f.admin, PurgeTrashCommand{ entry.id, TrashCondition{ entry.id, entry.revision } }));
    BOOST_REQUIRE(auditTransferMetadata(f.store(), f.resources.clock->now()));
    const auto now = f.resources.clock->now();
    {
        Runtime runtime(f, now);
        BOOST_REQUIRE(runtime.recover());
    }
    f.reopen();
    BOOST_CHECK(f.node(copy.metadata.id).metadata.id == copy.metadata.id);
    auto read = take(f.store().beginDriveRead());
    BOOST_REQUIRE(take(read->uploads().find(u.id))->resultDeletedAt);
    BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 3);
}
BOOST_AUTO_TEST_CASE(gc_respects_read_lease_then_reclaims_once) {
    Fixture f;
    auto b = purge(f, publishFile(f));
    {
        Runtime r(f, take(blobGcDeadline(*b.unreferencedAt)));
        BOOST_REQUIRE(r.recover());
        auto lease = r.uploads.blobAccess().tryAcquireRead(b.content.id);
        BOOST_REQUIRE(lease);
        r.tick();
        BOOST_CHECK(std::filesystem::exists(f.root / ".anof" / b.content.storageKey()));
        lease.reset();
        r.tick();
        BOOST_CHECK(!r.maintenance.paused());
    }
    settled(f, b.content.id);
}
BOOST_AUTO_TEST_CASE(unknown_mark_final_commit_and_disk_effect_are_reconciled) {
    for (unsigned stage = 0; stage < 5; ++stage) {
        BOOST_TEST_CONTEXT("stage " << stage) {
            Fixture f;
            auto b = purge(f, publishFile(f));
            {
                Runtime r(f, take(blobGcDeadline(*b.unreferencedAt)));
                BOOST_REQUIRE(r.recover());
                if (stage == 0) r.fault.commit.armed = true;
                if (stage == 1) r.fault.armFinal = true;
                if (stage == 2) r.fault.unknownDisk = true;
                if (stage == 3) {
                    r.fault.commit.afterCommit = false;
                    r.fault.commit.armed = true;
                }
                if (stage == 4) {
                    r.fault.commit.afterCommit = false;
                    r.fault.armFinal = true;
                }
                r.tick();
                if (stage == 2) {
                    f.reopen();
                    auto read = take(f.store().beginDriveRead());
                    auto row = take(read->blobLifecycle().record(b.content.id));
                    BOOST_REQUIRE(row);
                    BOOST_CHECK(row->state == BlobState::Deleting);
                    BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 3);
                }
                r.tick();
                BOOST_CHECK(!r.maintenance.paused());
            }
            settled(f, b.content.id);
        }
    }
}
BOOST_AUTO_TEST_CASE(unreadable_unknown_mark_blocks_leases_and_pauses) {
    Fixture f;
    auto b = purge(f, publishFile(f));
    {
        Runtime r(f, take(blobGcDeadline(*b.unreferencedAt)));
        BOOST_REQUIRE(r.recover());
        r.fault.commit.armed = true;
        r.fault.failReopen = true;
        r.tick();
        BOOST_CHECK(r.maintenance.paused());
        BOOST_CHECK(!r.uploads.blobAccess().tryAcquireRead(b.content.id));
        BOOST_CHECK(std::filesystem::exists(f.root / ".anof" / b.content.storageKey()));
    }
    {
        Runtime r(f, take(blobGcDeadline(*b.unreferencedAt)));
        BOOST_REQUIRE(r.recover());
    }
    settled(f, b.content.id);
}
BOOST_AUTO_TEST_CASE(stop_consumes_inflight_disk_before_workers_and_lease_retire) {
    Fixture f;
    auto b = purge(f, publishFile(f));
    {
        Runtime r(f, take(blobGcDeadline(*b.unreferencedAt)));
        BOOST_REQUIRE(r.recover());
        r.fault.blockDisk = true;
        r.maintenance.tick();
        r.wait([&] { return r.fault.entered.load(); });
        bool stopped = false;
        r.maintenance.requestStop([&] { stopped = true; });
        BOOST_CHECK(!stopped);
        BOOST_CHECK(!r.uploads.blobAccess().tryAcquireRead(b.content.id));
        r.fault.open();
        r.wait([&] { return stopped; });
        BOOST_CHECK(!r.maintenance.busy());
    }
    settled(f, b.content.id);
}
BOOST_AUTO_TEST_CASE(process_exit_at_gc_boundaries_recovers_real_files_without_double_charge) {
    for (unsigned stage = 0; stage < 5; ++stage) {
        BOOST_TEST_CONTEXT("boundary " << stage) {
            Fixture f;
            auto b = purge(f, publishFile(f));
            auto now = take(blobGcDeadline(*b.unreferencedAt));
            f.resources = {};
            auto pid = fork();
            BOOST_REQUIRE(pid >= 0);
            if (pid == 0) {
                auto resource = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3");
                if (!resource) _exit(91);
                auto* store = dynamic_cast<DriveMetadataStore*>(resource.value().metadata.get());
                if (!store) _exit(92);
                FixedClock clock(now);
                MaintenanceService service(*store, clock);
                if (stage == 0) _exit(73);
                auto marked = service.mark(b.content.id, b.revision, request());
                if (!marked) _exit(93);
                if (stage >= 2) {
                    struct ExitAfterUnlink : infrastructure::storage::detail::FileOperations {
                        int remove(int fd, const char* name) override {
                            auto result = FileOperations::remove(fd, name);
                            _exit(result == 0 ? 73 : 95);
                        }
                    };
                    auto fs = stage == 2 ? infrastructure::storage::detail::openLocalTransferFileStore(f.root, std::make_shared<ExitAfterUnlink>())
                                         : infrastructure::storage::openLocalTransferFileStore(f.root);
                    if (!std::holds_alternative<std::unique_ptr<TransferFileStore>>(fs)) _exit(94);
                    auto removed = std::get<std::unique_ptr<TransferFileStore>>(fs)->removeAndSync(ObjectFile{ b.content.id });
                    if (!std::holds_alternative<std::monostate>(removed)) _exit(95);
                }
                if (stage == 4 && !service.finalize(b.content.id, marked.value().revision, request())) _exit(96);
                _exit(73);  // 不运行C++析构，保留真实持久边界供父进程恢复。
            }
            int status = 0;
            BOOST_REQUIRE(waitpid(pid, &status, 0) == pid);
            BOOST_REQUIRE(WIFEXITED(status));
            BOOST_REQUIRE_EQUAL(WEXITSTATUS(status), 73);
            {
                Runtime r(f, now);
                BOOST_REQUIRE(r.recover());
                if (stage == 0) r.tick();
            }
            settled(f, b.content.id);
        }
    }
}
BOOST_AUTO_TEST_CASE(available_missing_object_is_not_hidden_by_gc) {
    Fixture f;
    auto b = purge(f, publishFile(f));
    fileTake(files(f.root)->removeAndSync(ObjectFile{ b.content.id }));
    Runtime r(f, take(blobGcDeadline(*b.unreferencedAt)));
    auto result = r.recover();
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == ErrorCode::IntegrityError);
}
BOOST_AUTO_TEST_CASE(tick_limits_expiry_and_continues_cursor_without_live_session) {
    Fixture f;
    std::optional<UtcTime> due;
    for (unsigned i = 0; i < 17; ++i) {
        auto n = f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "expired-" + std::to_string(i));
        due = trash(f, n.metadata.id).expiresAt;
    }
    {
        Runtime r(f, *due);
        BOOST_REQUIRE(r.recover());
        r.tick();
        f.reopen();
        {
            auto read = take(f.store().beginDriveRead());
            auto page = take(read->trash().expired(*due, PageSize::from(16).value(), {}));
            BOOST_CHECK_EQUAL(page.items.size(), 1);
        }
        r.tick();
    }
    f.reopen();
    auto read = take(f.store().beginDriveRead());
    BOOST_CHECK(take(read->trash().expired(*due, PageSize::from(16).value(), {})).items.empty());
    BOOST_CHECK_EQUAL(take(read->nodeAccounting().get()).used.value(), 2);
}
BOOST_AUTO_TEST_CASE(grace_boundary_and_transient_deleting_failure_preserve_responsibility) {
    Fixture f;
    auto b = purge(f, publishFile(f));
    auto due = take(blobGcDeadline(*b.unreferencedAt));
    {
        Runtime r(f, due - std::chrono::microseconds(1));
        BOOST_REQUIRE(r.recover());
        r.tick();
    }
    f.reopen();
    {
        auto read = take(f.store().beginDriveRead());
        auto row = take(read->blobLifecycle().record(b.content.id));
        BOOST_REQUIRE(row);
        BOOST_CHECK(row->state == BlobState::Available);
        BOOST_CHECK(row->unreferencedAt == b.unreferencedAt);
    }
    {
        Runtime r(f, due);
        BOOST_REQUIRE(r.recover());
        r.fault.failDisk = true;
        r.tick();
    }
    f.reopen();
    {
        auto read = take(f.store().beginDriveRead());
        auto row = take(read->blobLifecycle().record(b.content.id));
        BOOST_REQUIRE(row);
        BOOST_CHECK(row->state == BlobState::Deleting);
        BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 3);
    }
    {
        Runtime r(f, due);
        r.fault.failDisk = true;
        BOOST_REQUIRE(r.recover());  // 归属已验证的临时I/O失败允许ready，保留DELETING。
        r.tick();
    }
    settled(f, b.content.id);
}
BOOST_AUTO_TEST_CASE(gc_candidate_page_is_bounded_and_failed_id_does_not_starve_following_objects) {
    Fixture f;
    std::optional<UtcTime> due;
    for (unsigned i = 0; i < 17; ++i) {
        auto b = purge(f, publishFile(f, "blob-" + std::to_string(i)));
        due = take(blobGcDeadline(*b.unreferencedAt));
    }
    {
        Runtime r(f, *due);
        BOOST_REQUIRE(r.recover());
        r.fault.failDisk = true;
        r.tick();
        f.reopen();
        {
            auto read = take(f.store().beginDriveRead());
            auto gc = take(read->blobLifecycle().gcStatistics());
            BOOST_CHECK_EQUAL(gc.pendingObjects.value(), 2);
            BOOST_CHECK_EQUAL(gc.deletingObjects.value(), 1);
        }
        r.tick();
    }
    f.reopen();
    auto read = take(f.store().beginDriveRead());
    BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 0);
}
BOOST_AUTO_TEST_CASE(download_read_lease_covers_stream_and_head_retires_before_completion) {
    Fixture f;
    auto u = publishFile(f);
    const auto now = f.resources.clock->now();
    Runtime r(f, now);
    BOOST_REQUIRE(r.recover());
    auto open = [&](bool head) {
        std::optional<Result<OpenedDownload>> result;
        auto accepted = r.uploads.submit(*f.admin, OpenDownloadCommand{ *u.resultNodeId, head }, [&](TaskCompletion<OpenedDownload> c) {
            result = std::move(std::get<Result<OpenedDownload>>(c.outcome));
        });
        BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(accepted));
        r.wait([&] { return result.has_value(); });
        return take(std::move(*result));
    };
    {
        auto head = open(true);
        BOOST_CHECK(!head.stream);
        auto lease = r.uploads.blobAccess().tryAcquireDelete(*u.pendingBlobId);
        BOOST_REQUIRE(lease);
    }
    auto download = open(false);
    BOOST_REQUIRE(download.stream);
    BOOST_CHECK(!r.uploads.blobAccess().tryAcquireDelete(*u.pendingBlobId));
    f.reopen();
    auto b = purge(f, u);  // 已授权下载不因入站/永久删除追溯取消。
    auto block = std::make_unique<TransferBlock>();
    std::optional<Result<OwnedTransferBlock>> pulled;
    auto accepted = download.stream->pull({}, 3, block, [&](TaskCompletion<OwnedTransferBlock> c) { pulled = std::move(std::get<Result<OwnedTransferBlock>>(c.outcome)); });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(accepted));
    r.wait([&] { return pulled.has_value(); });
    BOOST_REQUIRE(*pulled);
    BOOST_CHECK_EQUAL(pulled->value()->bytes[0], 'a');
    download.stream->consumed(3);
    download.stream->cancel();
    r.wait([&] { return !r.uploads.blobPinned(b.content.id); });
    auto lease = r.uploads.blobAccess().tryAcquireDelete(b.content.id);
    BOOST_REQUIRE(lease);
}

BOOST_AUTO_TEST_CASE(download_old_snapshot_is_rechecked_after_read_lease_acquisition) {
    Fixture f;
    auto u = publishFile(f);
    auto now = f.resources.clock->now();
    Runtime r(f, now);
    BOOST_REQUIRE(r.recover());
    // 退休启动扫描，闸门只能截获本次下载鉴权。
    r.wait([&] { return r.metadata.snapshot().outstanding == 0; });
    r.fault.blockClock = true;
    std::optional<Result<OpenedDownload>> result;
    auto accepted = r.uploads.submit(*f.admin, OpenDownloadCommand{ *u.resultNodeId, false }, [&](TaskCompletion<OpenedDownload> c) {
        result = std::move(std::get<Result<OpenedDownload>>(c.outcome));
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(accepted));
    r.wait([&] { return r.fault.entered.load(); });
    bool removed = false;
    auto mutation = r.metadata.submitOperation([&](MetadataWorkerResources& resources) -> Result<void> {
        auto* store=dynamic_cast<DriveMetadataStore*>(resources.metadata.get()); if(!store) return ErrorCode::InternalError;
        DriveService service(*store,*resources.clock,*resources.nodeIds,*resources.driveIds);
        auto read=store->beginDriveRead(); if(!read) return read.error();
        auto node=read.value()->nodes().find(*u.resultNodeId); if(!node || !node.value()) return ErrorCode::IntegrityError;
        auto revision=node.value()->metadata.revision; read.value().reset();
        auto entry=service.execute(*f.admin,TrashNodeCommand{*u.resultNodeId,NodeCondition{*u.resultNodeId,revision}}); if(!entry) return entry.error();
        return service.execute(*f.admin,PurgeTrashCommand{entry.value().entry.id,TrashCondition{entry.value().entry.id,entry.value().entry.revision}}); }, [&](TaskCompletion<void> c) {BOOST_REQUIRE(std::get<Result<void>>(c.outcome));removed=true; });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(mutation));
    r.fault.open();
    r.wait([&] { return result.has_value() && removed; });
    BOOST_REQUIRE(!*result);
    BOOST_CHECK(result->error().code == ErrorCode::NotFound);
    BOOST_CHECK(!r.uploads.blobPinned(*u.pendingBlobId));
}

BOOST_AUTO_TEST_CASE(zero_byte_gc_removes_object_and_row_without_physical_revision_change) {
    Fixture f;
    auto b = purge(f, publishFile(f, "empty", 0));
    Revision before;
    {
        auto read = take(f.store().beginDriveRead());
        before = take(read->storage().get()).revision;
        auto stats = take(read->blobLifecycle().gcStatistics());
        BOOST_CHECK_EQUAL(stats.pendingObjects.value(), 1);
        BOOST_CHECK_EQUAL(stats.pendingBytes.value(), 0);
    }
    {
        Runtime r(f, take(blobGcDeadline(*b.unreferencedAt)));
        BOOST_REQUIRE(r.recover());
        r.tick();
    }
    settled(f, b.content.id);
    auto read = take(f.store().beginDriveRead());
    BOOST_CHECK(take(read->storage().get()).revision == before);
}
BOOST_AUTO_TEST_CASE(deleting_object_with_wrong_length_fails_before_destructive_recovery) {
    Fixture f;
    auto b = purge(f, publishFile(f));
    auto due = take(blobGcDeadline(*b.unreferencedAt));
    FixedClock clock(due);
    MaintenanceService service(f.store(), clock);
    take(service.mark(b.content.id, b.revision, request()));
    {
        std::ofstream changed(f.root / ".anof" / b.content.storageKey(), std::ios::binary | std::ios::app);
        changed.put('x');
        BOOST_REQUIRE(changed.good());
    }
    Runtime r(f, due);
    auto result = r.recover();
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == ErrorCode::IntegrityError);
    BOOST_CHECK(std::filesystem::exists(f.root / ".anof" / b.content.storageKey()));
}
BOOST_AUTO_TEST_CASE(instance_ready_waits_for_deleting_recovery_and_keeps_lease_until_destruction) {
    Fixture f;
    auto b = purge(f, publishFile(f));
    auto due = take(blobGcDeadline(*b.unreferencedAt));
    FixedClock clock(due);
    MaintenanceService service(f.store(), clock);
    take(service.mark(b.content.id, b.revision, request()));
    fileTake(files(f.root)->removeAndSync(ObjectFile{ b.content.id }));
    f.resources = {};
    struct Lease : InstanceLease {
        std::filesystem::path path;
        bool& held;
        Lease(std::filesystem::path p, bool& h) : path(std::move(p)), held(h) { held = true; }
        ~Lease() override { held = false; }
        const std::filesystem::path& root() const noexcept override { return path; }
    };
    muduox::net::EventLoop loop;
    bool held = false, ready = false, stopped = false;
    InstanceConfiguration config;
    config.developmentHttp = true;
    config.port = 0;
    config.shutdownGraceSeconds = 0;
    auto server = take(makeInstanceServer(loop, { std::make_unique<Lease>(f.root, held), config }));
    struct Drain {
        muduox::net::EventLoop& loop;
        std::unique_ptr<InstanceServer>& server;
        ~Drain() {
            if (server) {
                server->requestStop();
                if (!anof::test::waitFor(loop, [&] { return server->state() == RuntimeState::Stopped; })) std::terminate();
                server.reset();
            }
        }
    } drain{ loop, server };
    BOOST_REQUIRE(server->start([&](auto result) {BOOST_REQUIRE(result);BOOST_CHECK(held);ready=true; }, [&] {BOOST_CHECK(held);stopped=true; }));
    BOOST_REQUIRE(anof::test::waitFor(loop, [&] { return ready; }));
    BOOST_CHECK(server->ready());
    settled(f, b.content.id);
    server->requestStop();
    BOOST_REQUIRE(anof::test::waitFor(loop, [&] { return stopped; }));
    BOOST_CHECK(held);
    server.reset();
    BOOST_CHECK(!held);
}

BOOST_AUTO_TEST_CASE(full_metadata_queue_defers_expiry_without_blocking_loop_or_losing_work) {
    Fixture f;
    auto entry = trash(f, *publishFile(f).resultNodeId);
    Runtime r(f, entry.expiresAt);
    BOOST_REQUIRE(r.recover());
    r.wait([&] { return r.metadata.snapshot().outstanding == 0; });
    r.fault.blockClock = true;
    unsigned completed = 0;
    auto callback = [&](TaskCompletion<void> result) {
        BOOST_REQUIRE(std::get<Result<void>>(result.outcome));
        ++completed;
    };
    auto first = r.metadata.submitOperation([](MetadataWorkerResources& resources) -> Result<void> {
        resources.clock->now();
        return {};
    },
                                            callback);
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(first));
    r.wait([&] { return r.fault.entered.load(); });
    for (unsigned i = 0; i < 4; ++i) {
        auto accepted = r.metadata.submitOperation([](auto&) -> Result<void> { return {}; }, callback);
        BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(accepted));
    }
    BOOST_CHECK_EQUAL(r.metadata.snapshot().pending, 4);
    BOOST_CHECK_EQUAL(r.metadata.snapshot().outstanding, 5);
    r.tick();  // worker仍由闸门阻塞，loop必须能消费拒绝并结束本次维护。
    BOOST_CHECK_EQUAL(completed, 0);
    BOOST_CHECK(!r.maintenance.paused());
    f.reopen();
    {
        auto read = take(f.store().beginDriveRead());
        BOOST_REQUIRE(take(read->trash().find(entry.id)));
        BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 3);
    }
    r.fault.open();
    r.wait([&] { return completed == 5 && r.metadata.snapshot().outstanding == 0; });
    r.tick();
    f.reopen();
    auto read = take(f.store().beginDriveRead());
    BOOST_CHECK(!take(read->trash().find(entry.id)));
    BOOST_CHECK_EQUAL(take(read->storage().get()).retained.value(), 3);
    BOOST_CHECK_EQUAL(take(read->nodeAccounting().get()).used.value(), 2);
}
