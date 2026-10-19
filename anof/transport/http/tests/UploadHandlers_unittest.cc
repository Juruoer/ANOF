#define BOOST_TEST_MODULE UploadHandlers
#include "UploadTestSupport.h"
#include <nlohmann/json.hpp>
using namespace anof;
using namespace anof::http;
using namespace anof::http::testing;
using Json = nlohmann::json;
namespace {
void status(const std::string& wire, int code) { BOOST_CHECK_MESSAGE(wire.find("HTTP/1.1 "+std::to_string(code)+" ")==0,wire); }
std::string body(const std::string& wire) { return wire.substr(wire.find("\r\n\r\n")+4); }
template<typename C,typename V,typename Value> std::string complete(UploadFixture& f, std::string request, Value value) {
    const auto index=f.dispatcher.jobs.size(); Client client(f.server.listenAddress()); client.send(request);
    f.until([&]{return f.dispatcher.jobs.size()==index+1;}); auto job=f.job<C,V>(index);
    BOOST_CHECK(job->context.credential.mode==CredentialMode::Bearer);
    job->result.emplace(std::move(value)); f.dispatcher.release(job);
    f.until([&]{client.read(); return client.eof;});
    BOOST_CHECK(client.bytes.find("x-request-id: "+job->context.requestId.toString())!=std::string::npos);
    BOOST_CHECK(client.bytes.find("cache-control: no-store")!=std::string::npos);
    return client.bytes;
}
}
BOOST_AUTO_TEST_CASE(controlRoutesAndHeadRepresentations) {
    UploadFixture f;
    for (auto disposition : {UploadCreateDisposition::Created,UploadCreateDisposition::Existing,UploadCreateDisposition::Creating}) {
        auto u=uploadView(); auto wire=complete<CreateUploadCommand,UploadCreated>(f,uploadRequest("POST","/api/v1/uploads",createUploadJson(),"Content-Type: application/json\r\n"),UploadCreated{u,disposition});
        status(wire,disposition==UploadCreateDisposition::Created?201:disposition==UploadCreateDisposition::Creating?202:200);
        BOOST_CHECK_EQUAL(Json::parse(body(wire))["id"],uploadId.toString());
    }
    auto wire=complete<ListUploadsCommand,Page<UploadView,UploadCursor>>(f,uploadRequest("GET","/api/v1/uploads?limit=1&client_upload_id="+operationId.toString()),Page<UploadView,UploadCursor>{{uploadView()}, {}});
    status(wire,200); BOOST_CHECK_EQUAL(Json::parse(body(wire))["items"].size(),1);
    auto list=f.job<ListUploadsCommand,Page<UploadView,UploadCursor>>(3); BOOST_CHECK(list->command.clientUploadId==operationId);
    status(complete<GetUploadCommand,UploadView>(f,uploadRequest("GET",uploadPath),uploadView()),200);
    wire=complete<GetUploadCommand,UploadView>(f,uploadRequest("HEAD",uploadPath),uploadView()); status(wire,200);
    BOOST_CHECK(body(wire).empty()); BOOST_CHECK(wire.find("Content-Length:")==std::string::npos); BOOST_CHECK(wire.find("content-type:")==std::string::npos);
    for(auto h:{"upload-offset: 0","upload-length: 200000","upload-state: OPEN","upload-expires: 1970-01-01T00:00:00.000000Z"}) BOOST_CHECK(wire.find(h)!=std::string::npos);
    wire=complete<GetUploadCommand,UploadView>(f,uploadRequest("HEAD",uploadPath),ErrorCode::NotFound); status(wire,404);
    BOOST_CHECK(body(wire).empty()); BOOST_CHECK(wire.find("Content-Length:")!=std::string::npos); BOOST_CHECK(wire.find("content-type: application/json")!=std::string::npos);
    wire=complete<CancelUploadCommand,void>(f,uploadRequest("DELETE",uploadPath),Result<void>{}); status(wire,204); BOOST_CHECK(body(wire).empty());
    wire=complete<CompleteUploadCommand,UploadCompletionAccepted>(f,uploadRequest("POST",uploadPath+"/complete","{}","Content-Type: application/json\r\n"),UploadCompletionAccepted{uploadView(),UploadCompleteDisposition::Accepted}); status(wire,202);
    status(complete<GetStorageCommand,StorageView>(f,uploadRequest("GET","/api/v1/admin/storage"),storageView()),200);
    wire=complete<SetStorageBudgetCommand,StorageView>(f,uploadRequest("PUT","/api/v1/admin/storage","{\"content_budget_bytes\":\"0\"}","Content-Type: application/json\r\nIf-Match: \"storage-"+instanceId.toString()+"-1\"\r\n"),storageView()); status(wire,200);
    BOOST_CHECK(wire.find("etag: \"storage-"+instanceId.toString()+"-1\"")!=std::string::npos);
}
BOOST_AUTO_TEST_CASE(patchPausesUntilPreparationAndRetiresOneOwnedBlockBeforeFinish) {
    UploadFixture f; Client client(f.server.listenAddress());
    const std::string payload(150000,'x');
    const auto input = patchRequest(payload);
    std::size_t sent = 0;
    f.until([&] {
        if (sent < input.size()) {
            const auto count = ::send(client.fd, input.data()+sent, input.size()-sent, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (count > 0) sent += static_cast<std::size_t>(count);
            else BOOST_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
        }
        return f.dispatcher.jobs.size()==1;
    });
    f.until([&]{return f.dispatcher.jobs.size()==1;}); auto job=f.job<PreparePatchCommand,PatchPreparation>();
    BOOST_CHECK_EQUAL(job->command.length.value(),150000); client.read(); BOOST_CHECK(client.bytes.empty());
    auto gate=f.prepare();
    while(gate->bytes<payload.size() || gate->feedCallback) {
        f.until([&]{
            if (sent < input.size()) {
                const auto count = ::send(client.fd, input.data()+sent, input.size()-sent, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (count > 0) sent += static_cast<std::size_t>(count);
                else BOOST_REQUIRE(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
            }
            return static_cast<bool>(gate->feedCallback);
        });
        BOOST_CHECK(gate->buffer); BOOST_CHECK_EQUAL(gate->finishes,0);
        for(std::size_t i=0;i<gate->buffer->size;++i) BOOST_REQUIRE(gate->buffer->bytes[i]=='x');
        gate->deliverFeed();
    }
    f.until([&]{return static_cast<bool>(gate->finishCallback);});
    BOOST_CHECK_EQUAL(gate->bytes,payload.size()); BOOST_CHECK(gate->feeds>=3); BOOST_CHECK(gate->maxBlock<=65536);
    client.read(); BOOST_CHECK(client.bytes.empty()); BOOST_CHECK_EQUAL(gate->finishes,1);
    gate->deliverFinish(UploadOffset::from(150000).value());
    f.until([&]{client.read();return client.eof;}); status(client.bytes,204);
    BOOST_CHECK(client.bytes.find("upload-offset: 150000")!=std::string::npos); BOOST_CHECK(body(client.bytes).empty());
    BOOST_CHECK(client.bytes.find("content-type:")==std::string::npos);
}
BOOST_AUTO_TEST_CASE(fragmentedPatchAndKeepAliveIsolation) {
    UploadFixture f; Client client(f.server.listenAddress()); auto request=patchRequest("abcdef",false);
    const auto end=request.find("\r\n\r\n")+4; client.send(request.substr(0,end)+"ab");
    f.until([&]{return f.dispatcher.jobs.size()==1;}); auto gate=f.prepare();
    f.until([&]{return static_cast<bool>(gate->feedCallback);}); BOOST_CHECK_EQUAL(gate->bytes,2);
    gate->deliverFeed(); client.send("cdef"); f.until([&]{return gate->feeds==2;}); gate->deliverFeed();
    f.until([&]{return static_cast<bool>(gate->finishCallback);}); gate->deliverFinish(UploadOffset::from(6).value());
    f.until([&]{client.read();return client.bytes.find("\r\n\r\n")!=std::string::npos;}); status(client.bytes,204);
    const auto boundary=client.bytes.size(); client.send(uploadRequest("GET",uploadPath));
    f.until([&]{return f.dispatcher.jobs.size()==2;}); auto job=f.job<GetUploadCommand,UploadView>(1); job->result.emplace(uploadView()); f.dispatcher.release(job);
    f.until([&]{client.read();return client.eof;}); status(client.bytes.substr(boundary),200);
    BOOST_CHECK_EQUAL(Json::parse(body(client.bytes.substr(boundary)))["id"],uploadId.toString());
}
BOOST_AUTO_TEST_CASE(prepareRejectionClosesUnconsumedBodyAndOffsetDetailsAreAuthorizedOnly) {
    UploadFixture f;
    auto input=patchRequest("abc",false)+uploadRequest("GET",uploadPath);
    auto conflict=PatchPreparation{PatchOffsetConflict{UploadOffset::from(17).value()}};
    auto wire=complete<PreparePatchCommand,PatchPreparation>(f,input,std::move(conflict)); status(wire,409);
    BOOST_CHECK_EQUAL(Json::parse(body(wire))["error"]["details"]["offset"],"17"); BOOST_CHECK_EQUAL(f.dispatcher.jobs.size(),1);
    wire=complete<PreparePatchCommand,PatchPreparation>(f,patchRequest("abc"),ErrorCode::UploadOffsetMismatch); status(wire,409); BOOST_CHECK(Json::parse(body(wire))["error"]["details"].empty());
    f.dispatcher.rejection=SubmitError::QueueFull; status(f.exchange(patchRequest("abc")),503);
}
BOOST_AUTO_TEST_CASE(patchFeedAndFinishRejectionAndFailureNeverConfirmOffset) {
    for(int mode=0;mode<4;++mode) {
        UploadFixture f; Client client(f.server.listenAddress()); client.send(patchRequest("abc"));
        f.until([&]{return f.dispatcher.jobs.size()==1;}); auto gate=f.prepare();
        if(mode==0) gate->feedRejection=SubmitError::QueueFull;
        if(mode==1) gate->finishRejection=SubmitError::Stopping;
        if(mode!=0) { f.until([&]{return static_cast<bool>(gate->feedCallback);}); gate->deliverFeed(mode==2?std::optional<ErrorCode>(ErrorCode::StorageFull):std::nullopt); }
        if(mode==3) { f.until([&]{return static_cast<bool>(gate->finishCallback);}); gate->deliverFinish(ErrorCode::CommitOutcomeUnknown); }
        f.until([&]{client.read();return client.eof;}); status(client.bytes,mode==3?500:503);
        BOOST_CHECK(client.bytes.find("upload-offset:")==std::string::npos); BOOST_CHECK(gate->cancelled);
    }
}
BOOST_AUTO_TEST_CASE(patchTimeoutAtPrepareFeedAndFinishConsumesLateResults) {
    for(int phase=0;phase<3;++phase) {
        UploadFixture f({128,15,30,0.04,0.04}); Client client(f.server.listenAddress()); client.send(patchRequest("abc"));
        f.until([&]{return f.dispatcher.jobs.size()==1;}); auto job=f.job<PreparePatchCommand,PatchPreparation>(); job->running=true;
        std::shared_ptr<PatchGate> gate;
        if(phase) {
            gate=f.prepare(); f.until([&]{return static_cast<bool>(gate->feedCallback);});
            if(phase==2) { gate->deliverFeed(); f.until([&]{return static_cast<bool>(gate->finishCallback);}); }
        }
        f.until([&]{client.read();return client.eof;}); const auto before=client.bytes;
        if(!phase) gate=f.prepare();
        else if(phase==1) gate->deliverFeed();
        else gate->deliverFinish(UploadOffset::from(3).value());
        f.until([&]{return gate->destroyed && (!gate->task || gate->task->delivered);});
        client.read(); BOOST_CHECK_EQUAL(client.bytes,before); BOOST_CHECK(client.bytes.find("204 No Content")==std::string::npos);
        BOOST_CHECK(gate->cancelled);
    }
}
BOOST_AUTO_TEST_CASE(forcedStopAtEveryPatchPhaseAndGraceAllowsExistingPatch) {
    for(int phase=0;phase<3;++phase) {
        UploadFixture f; Client client(f.server.listenAddress()); client.send(patchRequest("abc"));
        f.until([&]{return f.dispatcher.jobs.size()==1;}); f.job<PreparePatchCommand,PatchPreparation>()->running=true;
        std::shared_ptr<PatchGate> gate;
        if(phase) { gate=f.prepare(); f.until([&]{return static_cast<bool>(gate->feedCallback);});
            if(phase==2) {gate->deliverFeed(); f.until([&]{return static_cast<bool>(gate->finishCallback);});} }
        f.server.stop(0,[&]{f.stopped=true;}); f.until([&]{client.read();return f.stopped&&client.eof;});
        if(!phase) gate=f.prepare(); else if(phase==1) gate->deliverFeed(); else gate->deliverFinish(UploadOffset::from(3).value());
        f.until([&]{return gate->destroyed&&(!gate->task||gate->task->delivered);}); BOOST_CHECK(gate->cancelled);
    }
    UploadFixture f; Client client(f.server.listenAddress()); client.send(patchRequest("abc"));
    f.until([&]{return f.dispatcher.jobs.size()==1;}); auto gate=f.prepare(); f.until([&]{return static_cast<bool>(gate->feedCallback);});
    f.ready=false; f.server.stop(0.5,[&]{f.stopped=true;});
    gate->deliverFeed(); f.until([&]{return static_cast<bool>(gate->finishCallback);}); gate->deliverFinish(UploadOffset::from(3).value());
    f.until([&]{client.read();return client.eof&&f.stopped;}); status(client.bytes,204);
}
BOOST_AUTO_TEST_CASE(protocolRejectionsAndMethodsDoNotSubmit) {
    UploadFixture f;
    for(const auto& path:{std::string("/api/v1/uploads"),uploadPath,uploadPath+"/complete",std::string("/api/v1/admin/storage")}) status(f.exchange(uploadRequest("OPTIONS",path)),405);
    status(f.exchange(uploadRequest("GET",uploadPath,"x")),413);
    status(f.exchange(uploadRequest("GET","/api/v1/uploads?state=OPEN")),400);
    status(f.exchange(uploadRequest("POST","/api/v1/uploads","{}","Content-Type: application/json\r\n")),400);
    status(f.exchange(uploadRequest("PUT","/api/v1/admin/storage","{}","Content-Type: application/json\r\n")),428);
    auto input=patchRequest("abc"); const auto auth=input.find(uploadAuth); input.erase(auth,uploadAuth.size()); status(f.exchange(input),401);
    status(f.exchange(uploadRequest("PATCH",uploadPath,"abc","Content-Type: application/offset+octet-stream\r\n")),400);
    input="PATCH "+uploadPath+" HTTP/1.1\r\nHost: localhost\r\n"+uploadAuth+"Upload-Offset: 0\r\nContent-Type: application/offset+octet-stream\r\n\r\n"; status(f.exchange(input),411);
    status(f.exchange(uploadRequest("PATCH",uploadPath,"abc","Upload-Offset: 0\r\nContent-Type: multipart/form-data\r\n")),415);
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(expectContinueIsNotAuthorizationAndPrepareCanRejectWithoutBody) {
    UploadFixture f; Client client(f.server.listenAddress());
    auto request=patchRequest("abc"); request.insert(request.find("\r\n\r\n"),"\r\nExpect: 100-continue");
    client.send(request.substr(0,request.find("\r\n\r\n")+4));
    f.until([&]{client.read();return f.dispatcher.jobs.size()==1 && client.bytes.find("100 Continue")!=std::string::npos;});
    auto job=f.job<PreparePatchCommand,PatchPreparation>(); BOOST_CHECK(f.gates.empty());
    job->result.emplace(ErrorCode::Unauthenticated); f.dispatcher.release(job);
    f.until([&]{client.read();return client.eof;});
    BOOST_CHECK(client.bytes.find("401 Unauthorized")!=std::string::npos);
    BOOST_CHECK(client.bytes.find("upload-offset:")==std::string::npos);
    BOOST_CHECK(client.bytes.find("test-token")==std::string::npos);
}
BOOST_AUTO_TEST_CASE(patchFramingLimitsAndSecurityRejectBeforeDispatch) {
    UploadFixture f;
    auto header=[&](std::string fields) { return "PATCH "+uploadPath+" HTTP/1.1\r\nHost: localhost\r\n"+uploadAuth+fields+"\r\n"; };
    const std::string type="Content-Type: application/offset+octet-stream\r\nUpload-Offset: 0\r\n";
    status(f.exchange(header(type+"Content-Length: 8388609\r\n")),413);
    status(f.exchange(header(type+"Content-Length: 0\r\n")),400);
    status(f.exchange(header(type+"Transfer-Encoding: chunked\r\n")),415);
    status(f.exchange(header(type+"Transfer-Encoding: chunked\r\nContent-Length: 1\r\n")),400);
    status(f.exchange(header(type+"Content-Length: 1\r\nContent-Length: 1\r\n")),400);
    status(f.exchange(header(type+"Content-Length: 1\r\nTrailer: x\r\n")),400);
    status(f.exchange(header(type+"Content-Length: 1\r\nUpload-Offset: 0\r\n")),400);
    status(f.exchange(header(type+"Content-Length: 1\r\nCookie: a=b\r\n")),400);
    status(f.exchange(header("Content-Type: application/offset+octet-stream; charset=utf-8\r\nUpload-Offset: 0\r\nContent-Length: 1\r\n")),415);
    status(f.exchange(header(type+"Content-Length: 1\r\nContent-Encoding: gzip\r\n")),415);
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(disconnectedPeerAndLateTasksRetireWithoutAnotherExchange) {
    for(int phase=0;phase<3;++phase) {
        UploadFixture f({128,15,30,0.04,0.04}); Client client(f.server.listenAddress()); client.send(patchRequest("abc"));
        f.until([&]{return f.dispatcher.jobs.size()==1;}); f.job<PreparePatchCommand,PatchPreparation>()->running=true;
        std::shared_ptr<PatchGate> gate;
        if(phase) {gate=f.prepare(); f.until([&]{return static_cast<bool>(gate->feedCallback);});
            if(phase==2) {gate->deliverFeed();f.until([&]{return static_cast<bool>(gate->finishCallback);});}}
        ::shutdown(client.fd,SHUT_RDWR);
        // 暂停读取时 FIN 未必立刻可见；保留有限 HTTP 期限作为兜底。
        f.until([&]{return f.finished==1;});
        if(!phase) gate=f.prepare(); else if(phase==1) gate->deliverFeed(); else gate->deliverFinish(ErrorCode::CommitOutcomeUnknown);
        f.until([&]{return gate->destroyed&&(!gate->task||gate->task->delivered);});
        BOOST_CHECK(gate->cancelled); BOOST_CHECK_EQUAL(f.dispatcher.jobs.size(),1);
        status(complete<GetUploadCommand,UploadView>(f,uploadRequest("GET",uploadPath),uploadView()),200);
    }
}
BOOST_AUTO_TEST_CASE(controlErrorsHeadSuppressionAndStorageBindingPassThrough) {
    UploadFixture f;
    for(auto code:{ErrorCode::Unauthenticated,ErrorCode::Forbidden,ErrorCode::NotFound,ErrorCode::PreconditionFailed,ErrorCode::UploadTerminated,ErrorCode::UploadLimitReached,ErrorCode::StorageFull,ErrorCode::CommitOutcomeUnknown}) {
        const auto wire=complete<GetUploadCommand,UploadView>(f,uploadRequest("HEAD",uploadPath),code);
        BOOST_CHECK(body(wire).empty()); BOOST_CHECK(wire.find("Content-Length:")!=std::string::npos);
        BOOST_CHECK(wire.find("upload-offset:")==std::string::npos); BOOST_CHECK(wire.find("etag:")==std::string::npos);
    }
    for(auto rejection:{SubmitError::QueueFull,SubmitError::Stopping,SubmitError::RateLimited}) {
        f.dispatcher.rejection=rejection; status(f.exchange(uploadRequest("GET",uploadPath)),rejection==SubmitError::RateLimited?429:503);
    }
    f.dispatcher.rejection.reset();
    const auto other=InstanceId::parse("70000000-0000-4000-8000-000000000002").value();
    auto input=uploadRequest("PUT","/api/v1/admin/storage","{\"content_budget_bytes\":\"0\"}","Content-Type: application/json\r\nIf-Match: \"storage-"+other.toString()+"-2\"\r\n");
    status(complete<SetStorageBudgetCommand,StorageView>(f,input,ErrorCode::PreconditionFailed),412);
    auto job=f.job<SetStorageBudgetCommand,StorageView>(f.dispatcher.jobs.size()-1);
    BOOST_CHECK(job->command.condition.instance==other); BOOST_CHECK_EQUAL(job->command.condition.revision.value(),2);
}
BOOST_AUTO_TEST_CASE(newPatchRequiresReadyButAcceptedPreparationSurvivesGrace) {
    UploadFixture f; f.ready=false; status(f.exchange(patchRequest("abc")),503); BOOST_CHECK(f.dispatcher.jobs.empty());
    f.ready=true; Client client(f.server.listenAddress()); client.send(patchRequest("abc")); f.until([&]{return f.dispatcher.jobs.size()==1;});
    f.ready=false; f.server.stop(0.5,[&]{f.stopped=true;}); auto gate=f.prepare();
    f.until([&]{return static_cast<bool>(gate->feedCallback);}); gate->deliverFeed();
    f.until([&]{return static_cast<bool>(gate->finishCallback);}); gate->deliverFinish(UploadOffset::from(3).value());
    f.until([&]{client.read();return client.eof&&f.stopped;}); status(client.bytes,204);
}
