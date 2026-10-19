#define BOOST_TEST_MODULE DownloadExecutorIntegration
#include <tests/integration/DownloadFixture.h>

#include <fstream>
namespace {
Result<OpenedDownload> open(Fixture& f, const std::string& token, NodeId id, bool head = false) {
    std::optional<Result<OpenedDownload>> result;
    auto submission = f.server->transferDispatcher().submit(context(token), OpenDownloadCommand{ id, head }, [&](TaskCompletion<OpenedDownload> c) {
        result = std::move(std::get<Result<OpenedDownload>>(c.outcome));
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submission));
    BOOST_REQUIRE(drive(f.loop, [&] { return result.has_value(); }));
    return std::move(*result);
}
UploadExecutor& executor(Fixture& f) { return dynamic_cast<UploadExecutor&>(f.server->transferDispatcher()); }
Result<OwnedTransferBlock> pull(Fixture& f, DownloadStream& stream, UploadOffset offset, std::size_t count, OwnedTransferBlock& block) {
    std::optional<Result<OwnedTransferBlock>> result;
    auto submission = stream.pull(offset, count, block, [&](TaskCompletion<OwnedTransferBlock> c) {
        result = std::move(std::get<Result<OwnedTransferBlock>>(c.outcome));
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submission));
    BOOST_CHECK(!block);
    BOOST_REQUIRE(drive(f.loop, [&] { return result.has_value(); }));
    return std::move(*result);
}
}  // namespace
BOOST_AUTO_TEST_CASE(read_range_consumption_and_pin_lifetime) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = publish(f, token);
    auto opened = open(f, token, node(upload));
    BOOST_REQUIRE(opened && opened.value().stream);
    auto& stream = *opened.value().stream;
    auto cancelled = std::make_shared<unsigned>(0);
    stream.setCancellationCallback([cancelled] { ++*cancelled; });
    auto blob = opened.value().descriptor.blob.id;
    BOOST_CHECK(executor(f).blobPinned(blob));
    auto block = std::make_unique<TransferBlock>();
    auto* original = block.get();
    auto read = pull(f, stream, UploadOffset::from(1).value(), 2, block);
    BOOST_REQUIRE(read);
    block = std::move(read).value();
    BOOST_CHECK(block.get() == original);
    BOOST_CHECK_EQUAL(block->size, 2);
    BOOST_CHECK_EQUAL(block->bytes[0], 'b');
    BOOST_CHECK_EQUAL(block->bytes[1], 'c');
    auto denied = stream.pull(UploadOffset{}, 1, block, [](auto) {});
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(denied));
    BOOST_CHECK(block.get() == original);
    stream.consumed(1);
    denied = stream.pull(UploadOffset{}, 1, block, [](auto) {});
    BOOST_CHECK(std::holds_alternative<SubmitError>(denied));
    stream.consumed(1);
    read = pull(f, stream, UploadOffset{}, 1, block);
    BOOST_REQUIRE(read);
    block = std::move(read).value();
    BOOST_CHECK_EQUAL(block->bytes[0], 'a');
    stream.consumed(1);
    stream.cancel();
    stream.cancel();
    BOOST_CHECK_EQUAL(*cancelled, 1);
    BOOST_CHECK(executor(f).blobPinned(blob));
    BOOST_REQUIRE(drive(f.loop, [&] { return !executor(f).blobPinned(blob); }));
}
BOOST_AUTO_TEST_CASE(head_validates_without_slot_and_patch_shares_download_limits) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = publish(f, token);
    auto a = open(f, token, node(upload)), b = open(f, token, node(upload));
    BOOST_REQUIRE(a && b);
    auto denied = open(f, token, node(upload));
    BOOST_REQUIRE(!denied);
    BOOST_CHECK(denied.error().code == ErrorCode::Retryable);
    auto head = open(f, token, node(upload), true);
    BOOST_REQUIRE(head);
    BOOST_CHECK(!head.value().stream);
    auto zero = std::make_unique<TransferBlock>();
    auto rejected = a.value().stream->pull(UploadOffset::from(3).value(), 1, zero, [](auto) {});
    BOOST_CHECK(std::holds_alternative<SubmitError>(rejected));
    BOOST_CHECK(zero);
    // 创建仍可用，但PATCH必须与两条下载共享同一用户名额。
    auto pending = f.call("POST", "/api/v1/uploads", 201,
                          { { "client_upload_id", "10000000-0000-4000-8000-000000000002" }, { "space_id", upload["space_id"] }, { "parent_id", upload["parent_id"] }, { "name", "second" }, { "length", "1" } }, token);
    std::optional<Result<PatchPreparation>> patch;
    auto submission = f.server->transferDispatcher().submit(context(token), PreparePatchCommand{ UploadId::parse(pending["id"].get<std::string>()).value(), UploadOffset{}, ByteCount::from(1).value() },
                                                            [&](TaskCompletion<PatchPreparation> c) { patch = std::move(std::get<Result<PatchPreparation>>(c.outcome)); });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submission));
    BOOST_REQUIRE(drive(f.loop, [&] { return patch.has_value(); }));
    BOOST_REQUIRE(!*patch);
    BOOST_CHECK(patch->error().code == ErrorCode::Retryable);
}
BOOST_AUTO_TEST_CASE(cancel_queued_read_retires_pin_and_keeps_completion) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = publish(f, token);
    auto opened = open(f, token, node(upload));
    BOOST_REQUIRE(opened);
    auto block = std::make_unique<TransferBlock>();
    bool completed = false;
    auto submitted = opened.value().stream->pull(UploadOffset{}, 3, block, [&](TaskCompletion<OwnedTransferBlock> c) {
        BOOST_CHECK(std::holds_alternative<CancelledBeforeStart>(c.outcome));
        completed = true;
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submitted));
    BOOST_CHECK(!block);
    BOOST_CHECK(std::get<AcceptedTask>(submitted).handle->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    opened.value().stream.reset();
    BOOST_REQUIRE(drive(f.loop, [&] { return completed && !executor(f).blobPinned(opened.value().descriptor.blob.id); }));
}
BOOST_AUTO_TEST_CASE(corrupt_and_empty_objects_are_checked_before_metadata_success) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = publish(f, token);
    auto opened = open(f, token, node(upload));
    BOOST_REQUIRE(opened);
    const auto file = f.root / ".anof" / opened.value().descriptor.blob.storageKey();
    std::filesystem::resize_file(file, 1);
    auto head = open(f, token, node(upload), true);
    BOOST_REQUIRE(!head);
    BOOST_CHECK(head.error().code == ErrorCode::IntegrityError);
    auto block = std::make_unique<TransferBlock>();
    auto read = pull(f, *opened.value().stream, UploadOffset{}, 3, block);
    BOOST_REQUIRE(!read);
    BOOST_CHECK(read.error().code == ErrorCode::IntegrityError);
    // 仅清理本测试故障现场；不重启损坏实例。
}
BOOST_AUTO_TEST_CASE(empty_object_has_descriptor_and_never_reads_past_eof) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = publish(f, token, true);
    auto opened = open(f, token, node(upload));
    BOOST_REQUIRE(opened);
    BOOST_CHECK_EQUAL(opened.value().descriptor.blob.size.value(), 0);
    auto block = std::make_unique<TransferBlock>();
    auto result = opened.value().stream->pull(UploadOffset{}, 1, block, [](auto) {});
    BOOST_CHECK(std::holds_alternative<SubmitError>(result));
    BOOST_CHECK(block);
    auto directory = open(f, token, NodeId::parse(upload["parent_id"].get<std::string>()).value());
    BOOST_REQUIRE(!directory);
    BOOST_CHECK(directory.error().code == ErrorCode::NotAFile);
}
BOOST_AUTO_TEST_CASE(logout_does_not_revoke_open_read_but_disable_does) {
    Fixture f;
    f.start();
    auto admin = f.login();
    auto invite = f.call("POST", "/api/v1/admin/invites", 201, Json::object(), admin);
    auto user = f.call("POST", "/api/v1/auth/register", 201, registration("alice", invite["token"]));
    auto alice = f.login("alice");
    auto upload = publish(f, alice);
    auto hidden = open(f, admin, node(upload));
    BOOST_REQUIRE(!hidden);
    BOOST_CHECK(hidden.error().code == ErrorCode::NotFound);
    auto opened = open(f, alice, node(upload));
    BOOST_REQUIRE(opened);
    f.call("DELETE", "/api/v1/auth/session", 204, nullptr, alice);
    auto block = std::make_unique<TransferBlock>();
    auto read = pull(f, *opened.value().stream, UploadOffset{}, 3, block);
    BOOST_REQUIRE(read);
    block = std::move(read).value();
    opened.value().stream->consumed(3);
    const auto tag = "If-Match: \"user-" + user["id"].get<std::string>() + "-" + user["revision"].get<std::string>() + "\"\r\n";
    f.call("PATCH", "/api/v1/admin/users/" + user["id"].get<std::string>(), 200, { { "enabled", false } }, admin, tag);
    auto denied = opened.value().stream->pull(UploadOffset{}, 1, block, [](auto) {});
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(denied));
    BOOST_CHECK(std::get<SubmitError>(denied) == SubmitError::Stopping);
    BOOST_REQUIRE(drive(f.loop, [&] { return !executor(f).blobPinned(opened.value().descriptor.blob.id); }));
}

BOOST_AUTO_TEST_CASE(stop_waits_for_accepted_pull_and_releases_pin_before_unlock) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = publish(f, token);
    auto opened = open(f, token, node(upload));
    BOOST_REQUIRE(opened);
    auto block = std::make_unique<TransferBlock>();
    unsigned completed = 0;
    auto submission = opened.value().stream->pull(UploadOffset{}, 3, block, [&](TaskCompletion<OwnedTransferBlock>) { ++completed; });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submission));
    f.server->requestStop();
    f.locked();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.server->state() == RuntimeState::Stopped; }));
    BOOST_CHECK_EQUAL(completed, 1);
    BOOST_CHECK(!executor(f).blobPinned(opened.value().descriptor.blob.id));
    f.locked();
    opened.value().stream.reset();
    f.server.reset();
    auto locks = system::makeLinuxInstanceLockProvider();
    BOOST_REQUIRE(locks);
    BOOST_REQUIRE(locks.value()->acquire(f.root));
}
