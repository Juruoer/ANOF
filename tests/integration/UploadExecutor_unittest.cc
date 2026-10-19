#define BOOST_TEST_MODULE UploadExecutorIntegration
#include <anof/application/identity/IdentityService.h>
#include <anof/application/transfer/UploadService.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/storage/local/LocalTransferFileStore.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/runtime/UploadExecutor.h>
#include <muduox/net/EventLoop.h>
#include <sys/wait.h>
#include <tests/integration/InitializedInstance.h>
#include <tests/integration/TransferFaultStore.h>
#include <tests/support/EventLoopWait.h>
#include <unistd.h>

#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <csignal>
#include <fstream>
using namespace anof;
using namespace anof::runtime;
namespace {
RequestId request() { return RequestId::parse("12345678-1234-4234-8234-123456789abc").value(); }
struct Fixture {
    std::filesystem::path root;
    muduox::net::EventLoop loop;
    MetadataExecutor metadata{ loop };
    DiskExecutor disk{ loop };
    UploadExecutor uploads{ loop, metadata, disk, DiskSafetyPolicy{ ByteCount{}, SafetyPercent{} } };
    std::optional<RequestContext> context;
    std::optional<Space> space;
    Fixture(std::function<void(Fixture&)> seed = {}, bool expectRecovery = true, test::CommitFault* fault = nullptr) {
        char pattern[] = "/tmp/anof-upload-runtime-XXXXXX";
        auto* path = mkdtemp(pattern);
        BOOST_REQUIRE(path);
        root = path;
        BOOST_REQUIRE(anof::testing::initializeTestInstance(root));
        {
            auto r = makeInstanceMetadataResources(root / ".anof/metadata.sqlite3");
            BOOST_REQUIRE(r);
            IdentityService service(*r.value().metadata, *r.value().clock, *r.value().identity->ids,
                                    *r.value().identity->issuer, *r.value().identity->invites);
            LoginCommand command{ LoginName::parse("administrator").value(),
                                  PasswordInput::parse("fixture password only").value(), CredentialMode::Bearer,
                                  AuthenticationSource::ipv4({ 127, 0, 0, 1 }) };
            auto snapshot = service.prepareLogin(command.login);
            BOOST_REQUIRE(snapshot && snapshot.value());
            auto login = service.completeLogin(request(), command, snapshot.value(), true);
            BOOST_REQUIRE(login);
            auto codec = infrastructure::crypto::makeSodiumSessionTokenCodec();
            BOOST_REQUIRE(codec);
            auto digest = codec.value()->digestBearer(login.value().accessToken.view());
            BOOST_REQUIRE(digest);
            context = RequestContext{ { digest.value(), CredentialMode::Bearer }, request() };
            auto read = r.value().metadata->beginRead();
            BOOST_REQUIRE(read);
            auto actor =
                read.value()->authorization().requireActiveSession(context->credential, r.value().clock->now());
            BOOST_REQUIRE(actor);
            auto found = read.value()->spaces().findPrivate(actor.value().userId);
            BOOST_REQUIRE(found && found.value());
            space = *found.value();
        }
        if (seed) seed(*this);
        bool m = false, d = false;
        BOOST_REQUIRE(metadata.start(
            [this, fault]() -> Result<MetadataWorkerResources> {
                auto r = makeInstanceMetadataResources(root / ".anof/metadata.sqlite3");
                if (!r) return r.error();
                if (fault) {
                    auto* typed = dynamic_cast<TransferMetadataStore*>(r.value().metadata.get());
                    if (!typed) return ErrorCode::InternalError;
                    r.value().metadata.release();
                    r.value().metadata = std::make_unique<test::TransferFaultStore>(
                        std::unique_ptr<TransferMetadataStore>(typed), *fault);
                }
                return std::move(r).value();
            },
            [&](auto r) {
                BOOST_REQUIRE(r);
                m = true;
            }));
        BOOST_REQUIRE(disk.start(
            [this]() -> Result<std::unique_ptr<TransferFileStore>> {
                auto opened = infrastructure::storage::openLocalTransferFileStore(root);
                if (auto* value = std::get_if<std::unique_ptr<TransferFileStore>>(&opened)) return std::move(*value);
                return ErrorCode::IntegrityError;
            },
            [&](auto r) {
                BOOST_REQUIRE(r);
                d = true;
            },
            {}));
        BOOST_REQUIRE(test::waitFor(loop, [&] { return m && d; }));
        bool recovered = false;
        uploads.recover([&](auto r) {
            BOOST_CHECK(bool(r) == expectRecovery);
            recovered = true;
        });
        BOOST_REQUIRE(test::waitFor(loop, [&] { return recovered; }));
    }
    ~Fixture() {
        uploads.requestStop([this] { disk.requestStop(); });
        if (!test::waitFor(loop, [this] { return disk.snapshot().stopped; })) std::terminate();
        metadata.requestStop();
        if (!test::waitFor(loop, [this] { return metadata.snapshot().state == MetadataExecutorState::Stopped; }))
            std::terminate();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    template <class T, class Command>
    Result<T> run(Command command) {
        std::optional<Result<T>> result;
        auto accepted = uploads.submit(*context, std::move(command), [&](TaskCompletion<T> done) {
            if (auto* value = std::get_if<Result<T>>(&done.outcome))
                result = std::move(*value);
            else
                result = ErrorCode::Retryable;
        });
        BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(accepted));
        BOOST_REQUIRE(test::waitFor(loop, [&] { return result.has_value(); }));
        return std::move(*result);
    }
    CreateUploadCommand command(std::int64_t length = 3) {
        return { OperationId::parse("23456789-1234-4234-8234-123456789abc").value(),
                 { space->id, { space->rootNodeId, "payload" }, FileLength::from(length).value(), {} } };
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(create_patch_complete_and_replay) {
    Fixture f;
    auto command = f.command();
    auto created = f.run<UploadCreated>(command);
    BOOST_REQUIRE(created);
    BOOST_CHECK(created.value().disposition == UploadCreateDisposition::Created);
    auto id = created.value().upload.id;
    auto duplicate = f.run<UploadCreated>(command);
    BOOST_REQUIRE(duplicate);
    BOOST_CHECK(duplicate.value().upload.id == id);
    auto prepared = f.run<PatchPreparation>(PreparePatchCommand{ id, UploadOffset{}, ByteCount::from(3).value() });
    BOOST_REQUIRE(prepared);
    auto stream = std::move(std::get<PreparedPatch>(prepared.value()).stream);
    auto block = std::make_unique<TransferBlock>();
    block->size = 3;
    block->bytes[0] = 'a';
    block->bytes[1] = 'b';
    block->bytes[2] = 'c';
    bool fed = false;
    auto submission = stream->feed(block, [&](auto result) {
        auto& value = std::get<Result<OwnedTransferBlock>>(result.outcome);
        BOOST_REQUIRE(value);
        block = std::move(value).value();
        fed = true;
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submission));
    BOOST_CHECK(!block);
    BOOST_REQUIRE(test::waitFor(f.loop, [&] { return fed; }));
    bool committed = false;
    submission = stream->finish([&](auto done) {
        auto value = std::get<Result<UploadOffset>>(done.outcome);
        BOOST_REQUIRE(value);
        BOOST_CHECK_EQUAL(value.value().value(), 3);
        committed = true;
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submission));
    BOOST_REQUIRE(test::waitFor(f.loop, [&] { return committed; }));
    stream.reset();
    auto completion = f.run<UploadCompletionAccepted>(CompleteUploadCommand{ id, {} });
    BOOST_REQUIRE(completion);
    bool published = false;
    for (unsigned i = 0; i < 100 && !published; ++i) {
        auto state = f.run<UploadView>(GetUploadCommand{ id });
        BOOST_REQUIRE(state);
        published = state.value().state == UploadState::Completed;
    }
    BOOST_REQUIRE(published);
    auto storage = f.run<StorageView>(GetStorageCommand{});
    BOOST_REQUIRE(storage);
    BOOST_CHECK_EQUAL(storage.value().accounting.reserved.value(), 0);
    BOOST_CHECK_EQUAL(storage.value().accounting.retained.value(), 3);
    duplicate = f.run<UploadCreated>(command);
    BOOST_REQUIRE(duplicate);
    BOOST_CHECK(duplicate.value().upload.state == UploadState::Completed);
}
BOOST_AUTO_TEST_CASE(cancel_releases_reservations_only_after_cleanup) {
    Fixture f;
    auto created = f.run<UploadCreated>(f.command());
    BOOST_REQUIRE(created);
    auto id = created.value().upload.id;
    BOOST_REQUIRE(f.run<void>(CancelUploadCommand{ id }));
    bool cleaned = false;
    for (unsigned i = 0; i < 100 && !cleaned; ++i) {
        auto storage = f.run<StorageView>(GetStorageCommand{});
        BOOST_REQUIRE(storage);
        cleaned = storage.value().accounting.reserved.value() == 0;
    }
    BOOST_REQUIRE(cleaned);
    auto state = f.run<UploadView>(GetUploadCommand{ id });
    BOOST_REQUIRE(state);
    BOOST_CHECK(state.value().state == UploadState::Cancelled);
}
namespace {
Upload seedUpload(Fixture& f, UploadState desired, bool object = false, bool shortFile = false,
                  bool badDigest = false, bool confirmedOpen = false,
                  std::function<void(UploadService&, const Upload&)> boundary = {}) {
    auto resources = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(resources);
    auto& r = resources.value();
    UploadService service(dynamic_cast<TransferMetadataStore&>(*r.metadata), *r.clock, *r.transferIds, *r.nodeIds);
    auto finish = [&](const Upload& value) {
        if (boundary) boundary(service, value);
        return value;
    };
    auto command = f.command();
    if (badDigest) command.parameters.expectedSha256 = Sha256::parse(std::string(64, '0')).value();
    auto created = service.create(*f.context, command, ByteCount::from(1048576).value(), {});
    BOOST_REQUIRE(created);
    auto u = created.value().upload;
    if (desired == UploadState::Creating) return finish(u);
    auto fileStore = infrastructure::storage::openLocalTransferFileStore(f.root);
    BOOST_REQUIRE(std::holds_alternative<std::unique_ptr<TransferFileStore>>(fileStore));
    auto& fs = *std::get<std::unique_ptr<TransferFileStore>>(fileStore);
    auto file = fs.create(u.id);
    BOOST_REQUIRE(std::holds_alternative<OwnedTransferFile>(file));
    auto& handle = *std::get<OwnedTransferFile>(file);
    TransferBlock block;
    block.size = shortFile ? 1 : 3;
    block.bytes[0] = 'a';
    block.bytes[1] = 'b';
    block.bytes[2] = 'c';
    BOOST_REQUIRE(std::holds_alternative<std::monostate>(fs.write(handle, {}, block)));
    BOOST_REQUIRE(std::holds_alternative<std::monostate>(fs.syncData(handle)));
    auto opened = service.opened(u.id, u.generation);
    BOOST_REQUIRE(opened);
    u = opened.value();
    if (desired == UploadState::Open && !shortFile && !confirmedOpen) return finish(u);  // 未提交尾部 3 字节，确认前缀为 0。
    auto patch = service.preparePatch(*f.context, { u.id, {}, ByteCount::from(3).value() });
    BOOST_REQUIRE(patch);
    const auto utcBefore = r.clock->now();
    const auto monotonicBefore = std::chrono::steady_clock::now();
    auto committed = service.commitPatch(*f.context, patch.value());
    const auto utcAfter = r.clock->now();
    BOOST_REQUIRE_MESSAGE(committed, "commitPatch failed: error_value=" << (committed ? -1 : static_cast<int>(committed.error().code))
        << " created_us=" << u.createdAt.time_since_epoch().count()
        << " previous_progress_us=" << u.lastProgressAt.time_since_epoch().count()
        << " utc_before_us=" << utcBefore.time_since_epoch().count()
        << " utc_after_us=" << utcAfter.time_since_epoch().count()
        << " elapsed_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - monotonicBefore).count());
    u = committed.value();
    if (shortFile || desired == UploadState::Open) return finish(u);
    auto accepted = service.acceptComplete(*f.context, { u.id, {} });
    BOOST_REQUIRE(accepted);
    u = accepted.value();
    if (desired == UploadState::Verifying) return finish(u);
    auto verified = service.recordVerification(
        u.id, u.generation, Sha256::parse("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").value(),
        request());
    BOOST_REQUIRE(verified);
    u = verified.value();
    if (desired == UploadState::Ready) return finish(u);
    auto intent = service.preparePublish(u.id, u.generation);
    BOOST_REQUIRE(intent);
    u = intent.value();
    std::get<OwnedTransferFile>(file).reset();
    if (object) BOOST_REQUIRE(std::holds_alternative<std::monostate>(fs.publish(u.id, *u.pendingBlobId)));
    return finish(u);
}
}  // namespace
BOOST_AUTO_TEST_CASE(recovery_creates_missing_empty_and_truncates_unconfirmed_tail) {
    for (auto state : { UploadState::Creating, UploadState::Open }) {
        std::optional<Upload> seeded;
        Fixture f([&](auto& fixture) { seeded = seedUpload(fixture, state); });
        auto current = f.run<UploadView>(GetUploadCommand{ seeded->id });
        BOOST_REQUIRE(current);
        BOOST_CHECK(current.value().state == UploadState::Open);
        BOOST_CHECK_EQUAL(current.value().offset.value(), 0);
        BOOST_CHECK_EQUAL(std::filesystem::file_size(f.root / ".anof/uploads" / (seeded->id.toString() + ".part")), 0);
    }
}
BOOST_AUTO_TEST_CASE(recovery_rejects_short_confirmed_prefix_and_preserves_file) {
    std::optional<Upload> seeded;
    Fixture f([&](auto& fixture) { seeded = seedUpload(fixture, UploadState::Open, false, true); }, false);
    BOOST_CHECK_EQUAL(std::filesystem::file_size(f.root / ".anof/uploads" / (seeded->id.toString() + ".part")), 1);
}
BOOST_AUTO_TEST_CASE(recovery_resumes_committing_before_or_after_rename) {
    for (bool moved : { false, true }) {
        std::optional<Upload> seeded;
        Fixture f([&](auto& fixture) { seeded = seedUpload(fixture, UploadState::Committing, moved); });
        auto current = f.run<UploadView>(GetUploadCommand{ seeded->id });
        BOOST_REQUIRE(current);
        BOOST_CHECK(current.value().state == UploadState::Completed);
        auto storage = f.run<StorageView>(GetStorageCommand{});
        BOOST_REQUIRE(storage);
        BOOST_CHECK_EQUAL(storage.value().accounting.retained.value(), 3);
        BOOST_CHECK_EQUAL(storage.value().accounting.reserved.value(), 0);
    }
}
BOOST_AUTO_TEST_CASE(recovery_rejects_unknown_inventory_without_deletion) {
    Fixture f(
        [](auto& fixture) {
            std::ofstream file(fixture.root / ".anof/uploads/unowned");
            file << "keep";
        },
        false);
    BOOST_CHECK_EQUAL(std::filesystem::file_size(f.root / ".anof/uploads/unowned"), 4);
}
BOOST_AUTO_TEST_CASE(queued_feed_cancellation_retires_lease_and_truncates_tail) {
    Fixture f;
    auto created = f.run<UploadCreated>(f.command());
    BOOST_REQUIRE(created);
    auto prepared =
        f.run<PatchPreparation>(PreparePatchCommand{ created.value().upload.id, {}, ByteCount::from(3).value() });
    BOOST_REQUIRE(prepared);
    auto stream = std::move(std::get<PreparedPatch>(prepared.value()).stream);
    auto block = std::make_unique<TransferBlock>();
    block->size = 3;
    bool cancelled = false;
    auto task =
        stream->feed(block, [&](auto done) { cancelled = std::holds_alternative<CancelledBeforeStart>(done.outcome); });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(task));
    BOOST_CHECK(std::get<AcceptedTask>(task).handle->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    BOOST_REQUIRE(test::waitFor(f.loop, [&] { return cancelled; }));
    stream.reset();
}
BOOST_AUTO_TEST_CASE(uncertain_patch_commit_uses_fresh_evidence_before_truncating) {
    for (bool durable : { false, true }) {
        test::CommitFault fault;
        fault.afterCommit = durable;
        Fixture f({}, true, &fault);
        auto created = f.run<UploadCreated>(f.command());
        BOOST_REQUIRE(created);
        auto id = created.value().upload.id;
        auto prepared = f.run<PatchPreparation>(PreparePatchCommand{ id, {}, ByteCount::from(3).value() });
        BOOST_REQUIRE(prepared);
        auto stream = std::move(std::get<PreparedPatch>(prepared.value()).stream);
        auto block = std::make_unique<TransferBlock>();
        block->size = 3;
        bool fed = false;
        BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(stream->feed(block, [&](auto done) {
            BOOST_REQUIRE(std::get<Result<OwnedTransferBlock>>(done.outcome));
            fed = true;
        })));
        BOOST_REQUIRE(test::waitFor(f.loop, [&] { return fed; }));
        fault.armed = true;
        bool finished = false;
        BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(stream->finish([&](auto done) {
            auto result = std::get<Result<UploadOffset>>(done.outcome);
            BOOST_REQUIRE(!result);
            BOOST_CHECK(result.error().code == ErrorCode::CommitOutcomeUnknown);
            finished = true;
        })));
        BOOST_REQUIRE(test::waitFor(f.loop, [&] { return finished; }));
        stream.reset();
        BOOST_CHECK_EQUAL(fault.calls.load(), 1);
        auto row = f.run<UploadView>(GetUploadCommand{ id });
        BOOST_REQUIRE(row);
        BOOST_CHECK_EQUAL(row.value().offset.value(), durable ? 3 : 0);
        BOOST_CHECK_EQUAL(std::filesystem::file_size(f.root / ".anof/uploads" / (id.toString() + ".part")),
                          durable ? 3 : 0);
    }
}
BOOST_AUTO_TEST_CASE(ready_name_conflict_keeps_reservation_and_retarget_publishes) {
    std::optional<Upload> seeded;
    Fixture f([&](auto& fixture) { seeded = seedUpload(fixture, UploadState::Ready); });
    bool made = false;
    auto submitted =
        f.metadata.trySubmit(*f.context, { f.space->id, f.space->rootNodeId, "payload" }, [&](auto result) {
            BOOST_REQUIRE(std::get<Result<CreateDirectoryResult>>(result.outcome));
            made = true;
        });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submitted));
    BOOST_REQUIRE(test::waitFor(f.loop, [&] { return made; }));
    auto conflict = f.run<UploadCompletionAccepted>(CompleteUploadCommand{ seeded->id, {} });
    BOOST_REQUIRE(!conflict);
    BOOST_CHECK(conflict.error().code == ErrorCode::NameConflict);
    auto before = f.run<StorageView>(GetStorageCommand{});
    BOOST_REQUIRE(before);
    BOOST_CHECK_EQUAL(before.value().accounting.reserved.value(), 3);
    BOOST_CHECK(before.value().disk.has_value());
    auto retried = f.run<UploadCompletionAccepted>(
        CompleteUploadCommand{ seeded->id, UploadTarget{ f.space->rootNodeId, "renamed" } });
    BOOST_REQUIRE(retried);
    bool published = false;
    for (unsigned i = 0; i < 100 && !published; ++i) {
        auto row = f.run<UploadView>(GetUploadCommand{ seeded->id });
        BOOST_REQUIRE(row);
        published = row.value().state == UploadState::Completed;
    }
    BOOST_REQUIRE(published);
}
BOOST_AUTO_TEST_CASE(checksum_mismatch_fails_without_publishing_and_releases_after_cleanup) {
    std::optional<Upload> seeded;
    Fixture f([&](auto& fixture) { seeded = seedUpload(fixture, UploadState::Verifying, false, false, true); });
    bool failed = false;
    for (unsigned i = 0; i < 100 && !failed; ++i) {
        auto row = f.run<UploadView>(GetUploadCommand{ seeded->id });
        BOOST_REQUIRE(row);
        failed = row.value().state == UploadState::Failed;
        if (failed) BOOST_CHECK(row.value().lastError == TransferFailure::ChecksumMismatch);
    }
    BOOST_REQUIRE(failed);
    bool released = false;
    for (unsigned i = 0; i < 100 && !released; ++i) {
        auto budget = f.run<StorageView>(GetStorageCommand{});
        BOOST_REQUIRE(budget);
        BOOST_CHECK_EQUAL(budget.value().accounting.retained.value(), 0);
        released = budget.value().accounting.reserved.value() == 0;
    }
    BOOST_REQUIRE(released);
}
BOOST_AUTO_TEST_CASE(control_capacity_counts_queued_cancellations_until_callback_retirement) {
    Fixture f;
    unsigned completions = 0;
    std::vector<std::unique_ptr<TaskHandle>> handles;
    for (unsigned i = 0; i < 128; ++i) {
        auto submitted = f.uploads.submit(*f.context, GetStorageCommand{}, [&](auto done) {
            BOOST_CHECK(std::holds_alternative<CancelledBeforeStart>(done.outcome));
            ++completions;
        });
        BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submitted));
        handles.push_back(std::move(std::get<AcceptedTask>(submitted).handle));
    }
    auto rejected = f.uploads.submit(*f.context, GetStorageCommand{}, [](auto) {});
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(rejected));
    BOOST_CHECK(std::get<SubmitError>(rejected) == SubmitError::QueueFull);
    for (auto& handle : handles) BOOST_CHECK(handle->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    BOOST_CHECK_EQUAL(completions, 0);
    BOOST_REQUIRE(test::waitFor(f.loop, [&] { return completions == 128; }));
    BOOST_REQUIRE(f.run<StorageView>(GetStorageCommand{}));
}
BOOST_AUTO_TEST_CASE(stop_during_queued_patch_preparation_cannot_create_a_live_stream) {
    Fixture f;
    auto created = f.run<UploadCreated>(f.command());
    BOOST_REQUIRE(created);
    bool completed = false, stopped = false;
    auto task = f.uploads.submit(
        *f.context, PreparePatchCommand{ created.value().upload.id, {}, ByteCount::from(3).value() }, [&](auto done) {
            auto& result = std::get<Result<PatchPreparation>>(done.outcome);
            BOOST_CHECK(!result);
            completed = true;
        });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(task));
    f.uploads.requestStop([&] {
        stopped = true;
        f.disk.requestStop();
    });
    BOOST_REQUIRE(test::waitFor(f.loop, [&] { return completed && stopped && f.disk.snapshot().stopped; }));
}
BOOST_AUTO_TEST_CASE(recovery_rejects_accounting_mismatch_without_resetting_budget) {
    Fixture f(
        [](auto& fixture) {
            auto r = makeInstanceMetadataResources(fixture.root / ".anof/metadata.sqlite3");
            BOOST_REQUIRE(r);
            auto& store = dynamic_cast<TransferMetadataStore&>(*r.value().metadata);
            auto tx = store.beginTransferWrite();
            BOOST_REQUIRE(tx);
            BOOST_REQUIRE(tx.value()->storage().reserve(ByteCount::from(3).value()));
            BOOST_REQUIRE(tx.value()->commit());
        },
        false);
    auto r = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(r);
    auto& store = dynamic_cast<TransferMetadataStore&>(*r.value().metadata);
    auto read = store.beginTransferRead();
    BOOST_REQUIRE(read);
    auto accounting = read.value()->storage().get();
    BOOST_REQUIRE(accounting);
    BOOST_CHECK_EQUAL(accounting.value().reserved.value(), 3);
}

// 在启动任何worker前fork；子进程用真实端口达到持久边界后SIGKILL，不执行资源析构。
// 父进程重新打开SQLite/文件并执行生产恢复；这验证进程崩溃，不模拟硬件掉电。
BOOST_AUTO_TEST_CASE(process_death_at_durable_boundaries_recovers_without_double_accounting) {
    enum class Boundary { Creating,
                          Tail,
                          Offset,
                          Verifying,
                          Ready,
                          Intent,
                          Renamed,
                          Published,
                          Cancelled,
                          Unlinked,
                          Cleaned };
    for (auto boundary : { Boundary::Creating, Boundary::Tail, Boundary::Offset, Boundary::Verifying,
                           Boundary::Ready, Boundary::Intent, Boundary::Renamed, Boundary::Published,
                           Boundary::Cancelled, Boundary::Unlinked, Boundary::Cleaned }) {
        BOOST_TEST_CONTEXT("boundary " << static_cast<int>(boundary)) {
            std::optional<Upload> before;
            Fixture f([&](auto& fixture) {
                const auto pid = fork();
                BOOST_REQUIRE(pid >= 0);
                if (pid == 0) {
                    try {
                        auto desired = UploadState::Open;
                        if (boundary == Boundary::Creating) desired = UploadState::Creating;
                        if (boundary == Boundary::Verifying) desired = UploadState::Verifying;
                        if (boundary == Boundary::Ready) desired = UploadState::Ready;
                        if (boundary == Boundary::Intent || boundary == Boundary::Renamed ||
                            boundary == Boundary::Published) desired = UploadState::Committing;
                        seedUpload(fixture, desired,
                                   boundary == Boundary::Renamed || boundary == Boundary::Published,
                                   false, false, boundary != Boundary::Tail,
                                   [&](UploadService& service, const Upload& upload) {
                                       if (boundary == Boundary::Published) {
                                           auto moved = service.markObject(upload.id, upload.generation);
                                           BOOST_REQUIRE(moved);
                                           BOOST_REQUIRE(service.commitPublish(upload.id, moved.value().generation, request()));
                                       }
                                       if (boundary == Boundary::Cancelled || boundary == Boundary::Unlinked ||
                                           boundary == Boundary::Cleaned) {
                                           auto cancelled = service.cancel(*fixture.context, upload.id);
                                           BOOST_REQUIRE(cancelled);
                                           if (boundary != Boundary::Cancelled) {
                                               auto opened = infrastructure::storage::openLocalTransferFileStore(fixture.root);
                                               BOOST_REQUIRE(std::holds_alternative<std::unique_ptr<TransferFileStore>>(opened));
                                               auto& files = *std::get<std::unique_ptr<TransferFileStore>>(opened);
                                               BOOST_REQUIRE(std::holds_alternative<std::monostate>(
                                                   files.removeAndSync(TemporaryFile{ upload.id })));
                                               if (boundary == Boundary::Cleaned)
                                                   BOOST_REQUIRE(service.confirmCleanup(upload.id, cancelled.value().generation));
                                           }
                                       }
                                       ::kill(::getpid(), SIGKILL);
                                       _exit(91);
                                   });
                    } catch (...) {
                        _exit(92);
                    }
                    _exit(93);
                }
                int status = 0;
                pid_t waited;
                do {
                    waited = waitpid(pid, &status, 0);
                } while (waited < 0 && errno == EINTR);
                BOOST_REQUIRE_EQUAL(waited, pid);
                BOOST_REQUIRE_MESSAGE(WIFSIGNALED(status), "child failed before planned SIGKILL: wait_status=" << status
                    << " exit_code=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1));
                BOOST_REQUIRE_EQUAL(WTERMSIG(status), SIGKILL);
                auto resources = makeInstanceMetadataResources(fixture.root / ".anof/metadata.sqlite3");
                BOOST_REQUIRE(resources);
                auto& r = resources.value();
                UploadService service(dynamic_cast<TransferMetadataStore&>(*r.metadata), *r.clock,
                                      *r.transferIds, *r.nodeIds);
                auto replay = service.findReplay(*fixture.context, fixture.command());
                BOOST_REQUIRE(replay && replay.value());
                before = *replay.value();
            });
            auto current = f.run<UploadView>(GetUploadCommand{ before->id });
            BOOST_REQUIRE(current);
            if (boundary == Boundary::Ready) {
                BOOST_REQUIRE(current.value().state == UploadState::Ready);
                auto retained = f.run<StorageView>(GetStorageCommand{});
                BOOST_REQUIRE(retained);
                BOOST_CHECK_EQUAL(retained.value().accounting.reserved.value(), 3);
                BOOST_REQUIRE(f.run<UploadCompletionAccepted>(CompleteUploadCommand{ before->id, {} }));
            }
            // VERIFYING恢复只保证重新入队，READY须显式complete；不把ready通知当成发布完成。
            if (boundary == Boundary::Verifying || boundary == Boundary::Ready) {
                for (unsigned i = 0; i < 200 && current.value().state != UploadState::Completed; ++i) {
                    current = f.run<UploadView>(GetUploadCommand{ before->id });
                    BOOST_REQUIRE(current);
                }
            }
            const bool cancelled = boundary == Boundary::Cancelled || boundary == Boundary::Unlinked ||
                                   boundary == Boundary::Cleaned;
            const bool open = boundary == Boundary::Creating || boundary == Boundary::Tail ||
                              boundary == Boundary::Offset;
            BOOST_CHECK(current.value().state == (cancelled ? UploadState::Cancelled : open ? UploadState::Open
                                                                                            : UploadState::Completed));
            auto storage = f.run<StorageView>(GetStorageCommand{});
            BOOST_REQUIRE(storage);
            BOOST_CHECK_EQUAL(storage.value().accounting.reserved.value(), open ? 3 : 0);
            BOOST_CHECK_EQUAL(storage.value().accounting.retained.value(), (!open && !cancelled) ? 3 : 0);
            if (open) {
                const auto size = boundary == Boundary::Offset ? 3 : 0;
                BOOST_CHECK_EQUAL(current.value().offset.value(), size);
                BOOST_CHECK_EQUAL(std::filesystem::file_size(
                                      f.root / ".anof/uploads" / (before->id.toString() + ".part")),
                                  size);
            } else if (!cancelled) {
                auto replay = f.run<UploadCreated>(f.command());
                BOOST_REQUIRE(replay);
                BOOST_CHECK(replay.value().upload.resultNodeId == current.value().resultNodeId);
                if (before->resultNodeId) BOOST_CHECK(before->resultNodeId == current.value().resultNodeId);
            } else {
                BOOST_CHECK(!std::filesystem::exists(f.root / ".anof/uploads" / (before->id.toString() + ".part")));
            }
        }
    }
}
