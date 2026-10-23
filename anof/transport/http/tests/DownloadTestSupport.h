#pragma once
#include "UploadTestSupport.h"
#include <anof/transport/http/internal/DownloadState.h>
namespace anof::http::internal {
// 只读取发送器已有的控制状态；不向生产代码加入指标或伪造 writer 状态。
struct DownloadStateTestAccess {
    static bool writeBlocked(const DownloadState& s) { return s.waiting_ == DownloadState::Wait::Write; }
    static std::size_t sent(const DownloadState& s) { return s.sent_; }
    static std::size_t pending(const DownloadState& s) { return s.buffer_ ? s.buffer_->size - s.blockOffset_ : 0; }
};
}
namespace anof::http::testing {
inline const std::string contentPath = "/api/v1/nodes/" + uploadParent.toString() + "/content";
inline DownloadDescriptor descriptor(std::int64_t size = 100000) {
    return {uploadParent, "家庭 ;\"%.txt", {BlobId::parse("90000000-0000-4000-8000-000000000001").value(), FileLength::from(size).value(), Sha256::parse(std::string(64,'a')).value()}};
}
struct ReadGate {
    EventLoop& loop;
    OwnedTransferBlock buffer;
    TransferBlock* original = nullptr;
    Completion<OwnedTransferBlock> callback;
    std::function<void()> cancellation;
    std::shared_ptr<UploadJob> task;
    std::size_t pulls = 0, consumedBytes = 0, unconsumed = 0, requested = 0, delivered = 0;
    std::uint64_t offset = 0;
    bool autoRead = false, cancelled = false, destroyed = false, immediateCancel = false, cancelOnConsume = false;
    std::optional<SubmitError> rejection;
    explicit ReadGate(EventLoop& owner) : loop(owner) {}
    void deliver(std::optional<ErrorCode> error = {}, bool shortRead = false) {
        BOOST_REQUIRE(callback); BOOST_REQUIRE(buffer);
        for (std::size_t i=0;i<requested;++i) buffer->bytes[i]=static_cast<std::uint8_t>('a'+(offset+i)%26);
        buffer->size=requested-(shortRead?1:0);
        unconsumed=buffer->size;
        auto cb=std::move(callback); auto owned=std::make_shared<OwnedTransferBlock>(std::move(buffer)); auto work=task;
        loop.queueInLoop([this,cb=std::move(cb),owned,work,error]() mutable {
            work->delivered=true; ++delivered;
            if(error) cb({work->id,Result<OwnedTransferBlock>(*error)});
            else cb({work->id,Result<OwnedTransferBlock>(std::move(*owned))});
        });
    }
    void cancelFromRuntime() {
        cancelled=true;
        auto notify=cancellation;
        if(notify) notify();
    }
};
class GateDownload final : public anof::DownloadStream {
public:
    std::shared_ptr<ReadGate> gate;
    explicit GateDownload(std::shared_ptr<ReadGate> value) : gate(std::move(value)) {}
    ~GateDownload() override { cancel(); gate->destroyed=true; }
    TaskSubmission pull(UploadOffset offset,std::size_t count,OwnedTransferBlock& block,Completion<OwnedTransferBlock> cb) override {
        gate->loop.assertInLoopThread();
        BOOST_REQUIRE(!gate->callback && !gate->buffer && gate->unconsumed==0);
        BOOST_REQUIRE(block && count>0 && count<=kTransferBlockBytes);
        if(gate->rejection) return *gate->rejection;
        if(!gate->original) gate->original=block.get();
        BOOST_CHECK(gate->original==block.get());
        ++gate->pulls; gate->requested=count; gate->offset=static_cast<std::uint64_t>(offset.value());
        gate->callback=std::move(cb); gate->buffer=std::move(block);
        gate->task=std::make_shared<UploadTypedJob<int,void>>(RequestContext{{SessionDigest(SessionDigest::Bytes{}),CredentialMode::Bearer},Identity{}.forExchange(1,1)},0,[](auto){});
        gate->task->id=100+gate->pulls; gate->task->running=true;
        auto result=AcceptedTask{std::make_unique<UploadHandle>(gate->task)};
        if(gate->autoRead) gate->deliver();
        return result;
    }
    void consumed(std::size_t count) noexcept override {
        BOOST_CHECK(count>0 && count<=gate->unconsumed);
        gate->consumedBytes+=count; gate->unconsumed-=count;
        if(gate->cancelOnConsume) gate->cancelFromRuntime();
    }
    void setCancellationCallback(std::function<void()> cb) override {
        gate->cancellation=std::move(cb);
        if(gate->immediateCancel && gate->cancellation) gate->cancelFromRuntime();
    }
    void cancel() noexcept override { gate->cancelled=true; }
};
class DownloadFixture {
public:
    EventLoop loop;
    Codec codec;
    Identity identity;
    UnexpectedIdentityDispatcher identities;
    UploadFake dispatcher{loop};
    bool ready=true,stopped=false;
    std::size_t finished=0;
    std::vector<std::shared_ptr<ReadGate>> gates;
    std::weak_ptr<internal::DownloadState> state;
    UnexpectedDriveDispatcher drives;
    HttpApi api;
    HttpServer server{&loop,InetAddress(0,true),"download-test"};
    explicit DownloadFixture(HttpApiOptions options={},bool component=false) : api(codec,identity,[this]{return ready;},
        [](auto,const auto&,const auto&) -> HttpStreamCallbacks {BOOST_ERROR("Unexpected mkdir");return {};},identities,dispatcher,drives,
        {{},[this](auto){++finished;}},options) {
        if(!component) api.configure(server);
        else {
            // 独立组件服务器使用同一生产状态和真实 writer，读取控制状态以证实 WouldBlock。
            server.setHttpStreamCallback([this](const auto& request,const auto& writer) {
                auto value=std::make_shared<internal::DownloadState>(dispatcher,
                    RequestContext{{SessionDigest(SessionDigest::Bytes{}),CredentialMode::Bearer},identity.forExchange(1,1)},
                    OpenDownloadCommand{uploadParent,request.method()==HttpRequest::kHead},request.headers(),[this]{return ready;},writer);
                state=value;return value->callbacks();
            });
            HttpOutputLimits limits;limits.highWaterBytes=65536;server.setOutputLimits(limits);
        }
        server.start();
    }
    void until(const std::function<bool()>& done,const char* file=__builtin_FILE(),int line=__builtin_LINE()) {
        BOOST_REQUIRE(anof::test::waitFor(loop,done,3,file,line));
    }
    ~DownloadFixture() {
        server.stop(0,[this]{stopped=true;});
        for(auto& job:dispatcher.jobs) dispatcher.release(job);
        for(auto& gate:gates) if(gate->callback) gate->deliver(ErrorCode::InternalError);
        until([&]{
            if(!stopped || dispatcher.deliveries!=dispatcher.jobs.size()) return false;
            for(auto& gate:gates) if(gate->task && !gate->task->delivered) return false;
            return true;
        });
    }
    auto job(std::size_t index=0) {
        auto value=std::dynamic_pointer_cast<UploadTypedJob<OpenDownloadCommand,OpenedDownload>>(dispatcher.jobs.at(index));
        BOOST_REQUIRE(value);return value;
    }
    std::shared_ptr<ReadGate> open(DownloadDescriptor value=descriptor(),std::size_t index=0,bool automatic=false) {
        auto gate=std::make_shared<ReadGate>(loop);gates.push_back(gate);gate->autoRead=automatic;
        auto work=job(index);work->running=true;
        work->result.emplace(OpenedDownload{std::move(value),work->command.metadataOnly?nullptr:std::make_unique<GateDownload>(gate)});
        dispatcher.release(work);return gate;
    }
    std::string exchange(std::string input) {
        Client client(server.listenAddress());client.send(input);until([&]{client.read();return client.eof;});return client.bytes;
    }
};
inline std::string contentRequest(std::string headers={},bool head=false,bool close=true) {
    return uploadRequest(head?"HEAD":"GET",contentPath,{},std::move(headers),close);
}
}  // namespace anof::http::testing
