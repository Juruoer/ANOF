#pragma once
#include <anof/transport/http/TransferResponse.h>
#include <muduox/net/EventLoop.h>
#include <tests/support/EventLoopWait.h>

#include "HttpTestSupport.h"
namespace anof::http::testing {
inline const auto uploadId = UploadId::parse("60000000-0000-4000-8000-000000000001").value();
inline const auto instanceId = InstanceId::parse("70000000-0000-4000-8000-000000000001").value();
inline const auto operationId = OperationId::parse("80000000-0000-4000-8000-000000000001").value();
inline const auto uploadSpace = SpaceId::parse("10000000-0000-4000-8000-000000000001").value();
inline const auto uploadParent = NodeId::parse("20000000-0000-4000-8000-000000000001").value();
inline const std::string uploadPath = "/api/v1/uploads/" + uploadId.toString();
inline const std::string uploadAuth = "Authorization: Bearer test-token\r\n";
inline UploadView uploadView() {
    return { instanceId, uploadId, operationId, uploadSpace, { uploadParent, "file.txt" }, FileLength::from(200000).value(), UploadOffset::from(0).value(), {}, {}, UploadState::Open, {}, UtcTime{}, UtcTime{}, UtcTime{}, {}, {} };
}
inline StorageView storageView() {
    return { instanceId, { ByteCount::from(1000000).value(), ByteCount::from(0).value(), ByteCount::from(0).value(), Revision::from(1).value() }, {} };
}
inline std::string createUploadJson() {
    return "{\"client_upload_id\":\"" + operationId.toString() + "\",\"space_id\":\"" + uploadSpace.toString() + "\",\"parent_id\":\"" + uploadParent.toString() + "\",\"name\":\"file.txt\",\"length\":\"200000\"}";
}
struct UploadJob {
    std::uint64_t id = 0;
    bool cancelled = false, running = false, released = false, delivered = false;
    virtual ~UploadJob() = default;
    virtual void deliver() = 0;
};
template <typename C, typename V>
struct UploadTypedJob : UploadJob {
    RequestContext context;
    C command;
    Completion<V> callback;
    std::optional<Result<V>> result;
    UploadTypedJob(RequestContext ctx, C cmd, Completion<V> cb) : context(ctx), command(std::move(cmd)), callback(std::move(cb)) {}
    void deliver() override {
        auto cb = std::move(callback);
        // 模拟已运行操作即使 HTTP 取消仍须完成；任务不会因句柄销毁自动取消。
        if (cancelled && !running)
            cb({ id, CancelledBeforeStart{} });
        else
            cb({ id, result ? std::move(*result) : Result<V>(ErrorCode::InternalError) });
        result.reset();
    }
};
struct UploadHandle : TaskHandle {
    std::shared_ptr<UploadJob> job;
    explicit UploadHandle(std::shared_ptr<UploadJob> value) : job(std::move(value)) {}
    std::uint64_t id() const noexcept override { return job->id; }
    CancelBeforeStartResult cancelBeforeStart() noexcept override {
        if (job->delivered || job->released) return CancelBeforeStartResult::AlreadyTerminal;
        if (job->running) return CancelBeforeStartResult::AlreadyRunning;
        job->cancelled = true;
        return CancelBeforeStartResult::Cancelled;
    }
};
class UploadFake final : public TransferDispatcher {
public:
    EventLoop& loop;
    std::vector<std::shared_ptr<UploadJob>> jobs;
    std::optional<SubmitError> rejection;
    std::size_t deliveries = 0;
    explicit UploadFake(EventLoop& owner) : loop(owner) {}
    template <typename C, typename V>
    TaskSubmission accept(RequestContext context, C cmd, Completion<V> callback) {
        loop.assertInLoopThread();
        if (rejection) return *rejection;
        auto job = std::make_shared<UploadTypedJob<C, V>>(context, std::move(cmd), std::move(callback));
        job->id = jobs.size() + 1;
        jobs.push_back(job);
        return AcceptedTask{ std::make_unique<UploadHandle>(job) };
    }
    void release(const std::shared_ptr<UploadJob>& job) {
        if (job->released) return;
        job->released = true;
        loop.queueInLoop([this, job] { job->delivered = true; job->deliver(); ++deliveries; });
    }
    TaskSubmission submit(RequestContext context, OpenDownloadCommand cmd, Completion<OpenedDownload> callback) override { return accept<OpenDownloadCommand, OpenedDownload>(context, std::move(cmd), std::move(callback)); }
    TaskSubmission submit(RequestContext context, CreateUploadCommand cmd, Completion<UploadCreated> callback) override { return accept<CreateUploadCommand, UploadCreated>(context, std::move(cmd), std::move(callback)); }
    TaskSubmission submit(RequestContext context, ListUploadsCommand cmd, Completion<Page<UploadView, UploadCursor>> callback) override { return accept<ListUploadsCommand, Page<UploadView, UploadCursor>>(context, std::move(cmd), std::move(callback)); }
    TaskSubmission submit(RequestContext context, GetUploadCommand cmd, Completion<UploadView> callback) override { return accept<GetUploadCommand, UploadView>(context, std::move(cmd), std::move(callback)); }
    TaskSubmission submit(RequestContext context, CancelUploadCommand cmd, Completion<void> callback) override { return accept<CancelUploadCommand, void>(context, std::move(cmd), std::move(callback)); }
    TaskSubmission submit(RequestContext context, CompleteUploadCommand cmd, Completion<UploadCompletionAccepted> callback) override { return accept<CompleteUploadCommand, UploadCompletionAccepted>(context, std::move(cmd), std::move(callback)); }
    TaskSubmission submit(RequestContext context, PreparePatchCommand cmd, Completion<PatchPreparation> callback) override { return accept<PreparePatchCommand, PatchPreparation>(context, std::move(cmd), std::move(callback)); }
    TaskSubmission submit(RequestContext context, GetStorageCommand cmd, Completion<StorageView> callback) override { return accept<GetStorageCommand, StorageView>(context, std::move(cmd), std::move(callback)); }
    TaskSubmission submit(RequestContext context, SetStorageBudgetCommand cmd, Completion<StorageView> callback) override { return accept<SetStorageBudgetCommand, StorageView>(context, std::move(cmd), std::move(callback)); }
};
// 逐块显式 gate，不做文件 IO；记录同时持有的唯一缓冲及取消后的退休。
struct PatchGate {
    EventLoop& loop;
    OwnedTransferBlock buffer;
    Completion<OwnedTransferBlock> feedCallback;
    Completion<UploadOffset> finishCallback;
    std::shared_ptr<UploadJob> task;
    TransferBlock* original = nullptr;
    std::size_t feeds = 0, finishes = 0, bytes = 0, maxBlock = 0;
    bool cancelled = false, destroyed = false;
    std::optional<SubmitError> feedRejection, finishRejection;
    explicit PatchGate(EventLoop& owner) : loop(owner) {}
    void deliverFeed(std::optional<ErrorCode> error = {}) {
        BOOST_REQUIRE(feedCallback);
        auto callback = std::move(feedCallback);
        auto block = std::move(buffer);
        auto job = task;
        loop.queueInLoop([cb = std::move(callback), owned = std::make_shared<OwnedTransferBlock>(std::move(block)), job, error]() mutable {
            job->delivered = true;
            if (error)
                cb({ job->id, Result<OwnedTransferBlock>(*error) });
            else
                cb({ job->id, Result<OwnedTransferBlock>(std::move(*owned)) });
        });
    }
    void deliverFinish(Result<UploadOffset> result) {
        BOOST_REQUIRE(finishCallback);
        auto cb = std::move(finishCallback);
        auto job = task;
        loop.queueInLoop([cb = std::move(cb), result, job]() mutable { job->delivered = true; cb({job->id, result}); });
    }
};
class GatePatch final : public PatchStream {
public:
    std::shared_ptr<PatchGate> gate;
    explicit GatePatch(std::shared_ptr<PatchGate> value) : gate(std::move(value)) {}
    ~GatePatch() override {
        cancel();
        gate->destroyed = true;
    }
    UploadGeneration generation() const noexcept override { return UploadGeneration::from(1).value(); }
    TaskSubmission feed(OwnedTransferBlock& block, Completion<OwnedTransferBlock> cb) override {
        gate->loop.assertInLoopThread();
        BOOST_REQUIRE(!gate->buffer && !gate->feedCallback && !gate->finishCallback);
        BOOST_REQUIRE(block);
        BOOST_REQUIRE(block->size > 0 && block->size <= kTransferBlockBytes);
        if (gate->feedRejection) return *gate->feedRejection;
        if (!gate->original) gate->original = block.get();
        BOOST_CHECK(gate->original == block.get());
        gate->bytes += block->size;
        gate->maxBlock = std::max(gate->maxBlock, block->size);
        ++gate->feeds;
        gate->buffer = std::move(block);
        gate->feedCallback = std::move(cb);
        gate->task = std::make_shared<UploadTypedJob<int, void>>(RequestContext{ { SessionDigest(SessionDigest::Bytes{}), CredentialMode::Bearer }, Identity{}.forExchange(1, 1) }, 0, [](auto) {});
        gate->task->id = 100 + gate->feeds;
        gate->task->running = true;
        return AcceptedTask{ std::make_unique<UploadHandle>(gate->task) };
    }
    TaskSubmission finish(Completion<UploadOffset> cb) override {
        BOOST_REQUIRE(!gate->buffer && !gate->feedCallback && !gate->finishCallback);
        ++gate->finishes;
        if (gate->finishRejection) return *gate->finishRejection;
        gate->finishCallback = std::move(cb);
        gate->task = std::make_shared<UploadTypedJob<int, void>>(RequestContext{ { SessionDigest(SessionDigest::Bytes{}), CredentialMode::Bearer }, Identity{}.forExchange(1, 1) }, 0, [](auto) {});
        gate->task->id = 10000;
        gate->task->running = true;
        return AcceptedTask{ std::make_unique<UploadHandle>(gate->task) };
    }
    void cancel() noexcept override { gate->cancelled = true; }
};
class UploadFixture {
public:
    EventLoop loop;
    Codec codec;
    Identity identity;
    UnexpectedIdentityDispatcher identities;
    UploadFake dispatcher{ loop };
    bool ready = true, stopped = false;
    std::size_t finished = 0;
    std::vector<std::shared_ptr<PatchGate>> gates;
    UnexpectedDriveDispatcher drives;
    HttpApi api;
    HttpServer server{ &loop, InetAddress(0, true), "upload-test" };
    explicit UploadFixture(HttpApiOptions options = {}) : api(codec, identity, [this] { return ready; }, [](auto, const auto&, const auto&) -> HttpStreamCallbacks { BOOST_ERROR("Unexpected mkdir"); return {}; }, identities, dispatcher, drives, developmentConfiguration(), { {}, [this](auto) { ++finished; } }, options) {
        api.configure(server);
        server.start();
    }
    void until(const std::function<bool()>& done, const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        BOOST_REQUIRE(anof::test::waitFor(loop, done, 3, file, line));
    }
    ~UploadFixture() {
        server.stop(0, [this] { stopped = true; });
        for (auto& job : dispatcher.jobs) dispatcher.release(job);
        for (auto& gate : gates) {
            if (gate->feedCallback) gate->deliverFeed();
            if (gate->finishCallback) gate->deliverFinish(ErrorCode::CommitOutcomeUnknown);
        }
        until([&] { return stopped && dispatcher.deliveries == dispatcher.jobs.size(); });
        // 保证迟到 gate 回调退休后才销毁 loop。
        until([&] { for (auto& g : gates) if (g->task && !g->task->delivered) return false; return true; });
    }
    template <typename C, typename V>
    auto job(std::size_t index = 0) {
        auto value = std::dynamic_pointer_cast<UploadTypedJob<C, V>>(dispatcher.jobs.at(index));
        BOOST_REQUIRE(value);
        return value;
    }
    std::shared_ptr<PatchGate> prepare(std::size_t index = 0) {
        auto value = job<PreparePatchCommand, PatchPreparation>(index);
        value->running = true;
        auto gate = std::make_shared<PatchGate>(loop);
        gates.push_back(gate);
        value->result.emplace(PatchPreparation{ PreparedPatch{ value->command.offset, UploadGeneration::from(1).value(), std::make_unique<GatePatch>(gate) } });
        dispatcher.release(value);
        return gate;
    }
    std::string exchange(std::string input) {
        Client client(server.listenAddress());
        client.send(input);
        until([&] { client.read(); return client.eof; });
        return client.bytes;
    }
};
inline std::string uploadRequest(std::string method, std::string path, std::string body = {}, std::string extra = {}, bool close = true) {
    return method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n" + uploadAuth + (close ? "Connection: close\r\n" : "") +
           "Content-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
}
inline std::string patchRequest(std::string body, bool close = true) {
    return uploadRequest("PATCH", uploadPath, std::move(body), "Upload-Offset: 0\r\nContent-Type: application/offset+octet-stream\r\n", close);
}
}  // namespace anof::http::testing
