#define BOOST_TEST_MODULE DownloadHandlers
#include "DownloadTestSupport.h"
#include <iostream>
using namespace anof;
using namespace anof::http;
using namespace anof::http::testing;
namespace {
void status(const std::string& wire,int code) { BOOST_CHECK_MESSAGE(wire.find("HTTP/1.1 "+std::to_string(code)+" ")==0,wire.substr(0,1024)); }
std::string body(const std::string& wire) { auto p=wire.find("\r\n\r\n");return p==std::string::npos?std::string{}:wire.substr(p+4); }
const std::string etag="\"sha256-"+std::string(64,'a')+"\"";
void submitted(DownloadFixture& f,Client& c,std::string input=contentRequest()) {
    const auto count=f.dispatcher.jobs.size();c.send(input);f.until([&]{return f.dispatcher.jobs.size()==count+1;});
}
}
BOOST_AUTO_TEST_CASE(fullGetDelayedReadsReuseOneBlockAndReportExactConsumption) {
    DownloadFixture f;Client c(f.server.listenAddress());submitted(f,c);
    c.read();BOOST_CHECK(c.bytes.empty());BOOST_CHECK(!f.job()->command.metadataOnly);
    auto g=f.open();f.until([&]{return bool(g->callback);});BOOST_CHECK_EQUAL(g->pulls,1);BOOST_CHECK_EQUAL(g->requested,65536);
    g->deliver();f.until([&]{c.read();return g->pulls==2;});BOOST_CHECK_EQUAL(g->consumedBytes,65536);BOOST_CHECK_EQUAL(g->offset,65536);BOOST_CHECK_EQUAL(g->requested,34464);
    g->deliver();f.until([&]{c.read();return c.eof;});status(c.bytes,200);auto payload=body(c.bytes);BOOST_REQUIRE_EQUAL(payload.size(),100000);
    bool correct=true;for(std::size_t i=0;i<payload.size();++i) correct=correct&&payload[i]==static_cast<char>('a'+i%26);BOOST_CHECK(correct);
    BOOST_CHECK_EQUAL(g->consumedBytes,100000);BOOST_CHECK(g->cancelled&&g->destroyed);BOOST_CHECK(!g->cancellation);
    for(auto value:{"Content-Length: 100000","content-type: application/octet-stream","accept-ranges: bytes","cache-control: private,no-store","x-content-type-options: nosniff","filename=\"download\"; filename*=UTF-8''%"}) BOOST_CHECK(c.bytes.find(value)!=std::string::npos);
    BOOST_CHECK(c.bytes.find("x-request-id: "+f.job()->context.requestId.toString())!=std::string::npos);
}
BOOST_AUTO_TEST_CASE(headAndEmptyFileNeverPullAndRangeUsesResolvedOffset) {
    for(int mode=0;mode<3;++mode) {
        DownloadFixture f;Client c(f.server.listenAddress());submitted(f,c,contentRequest("Range: bytes=2-4\r\n",mode==0));
        auto g=f.open(descriptor(mode==1?0:100000),0,true);f.until([&]{c.read();return c.eof;});status(c.bytes,mode==2?206:200);
        BOOST_CHECK(f.job()->command.metadataOnly==(mode==0));
        if(mode<2) {BOOST_CHECK(body(c.bytes).empty());BOOST_CHECK_EQUAL(g->pulls,0);}
        else {BOOST_CHECK_EQUAL(body(c.bytes),"cde");BOOST_CHECK_EQUAL(g->offset,2);BOOST_CHECK_EQUAL(g->consumedBytes,3);BOOST_CHECK(c.bytes.find("content-range: bytes 2-4/100000")!=std::string::npos);}
    }
}
BOOST_AUTO_TEST_CASE(conditionsCancelBeforeAnyPullAndSuppressCorrectHeaders) {
    for(bool head:{false,true}) for(int mode=0;mode<4;++mode) {
        DownloadFixture f;Client c(f.server.listenAddress());
        std::string h=mode==0?"If-None-Match: "+etag+"\r\n":mode==1?"If-Match: \"other\"\r\n":mode==2?"If-Match: invalid\r\n":"Range: bytes=100000-\r\n";
        submitted(f,c,contentRequest(h,head));auto g=f.open();f.until([&]{c.read();return c.eof;});status(c.bytes,mode==0?304:mode==1?412:mode==2?400:head?200:416);
        BOOST_CHECK_EQUAL(g->pulls,0);if(!head) BOOST_CHECK(g->cancelled&&g->destroyed);
        if(head||mode==0||mode==3) BOOST_CHECK(body(c.bytes).empty());
        if(mode==0) for(auto field:{"Content-Length:","content-type:","content-disposition:"}) BOOST_CHECK(c.bytes.find(field)==std::string::npos);
        if(mode==3&&!head) {BOOST_CHECK(c.bytes.find("Content-Length: 0")!=std::string::npos);BOOST_CHECK(c.bytes.find("content-range: bytes */100000")!=std::string::npos);}
    }
}
BOOST_AUTO_TEST_CASE(authorizedOpenPrecedesConditionAndHiddenMetadataErrors) {
    for(bool head:{false,true}) for(auto error:{ErrorCode::NotFound,ErrorCode::Unauthenticated,ErrorCode::Forbidden,ErrorCode::InternalError}) {
        DownloadFixture f;Client c(f.server.listenAddress());submitted(f,c,contentRequest("If-Match: invalid\r\nRange: bytes=100000-\r\n",head));
        auto j=f.job();j->result.emplace(error);f.dispatcher.release(j);f.until([&]{c.read();return c.eof;});
        status(c.bytes,error==ErrorCode::NotFound?404:error==ErrorCode::Unauthenticated?401:error==ErrorCode::Forbidden?403:500);
        for(auto field:{"etag:","content-range:","content-disposition:","accept-ranges:"}) BOOST_CHECK(c.bytes.find(field)==std::string::npos);
        BOOST_CHECK(c.bytes.find("content-type: application/json")!=std::string::npos);BOOST_CHECK(c.bytes.find("Content-Length:")!=std::string::npos);BOOST_CHECK(body(c.bytes).empty()==head);
    }
}
BOOST_AUTO_TEST_CASE(protocolRejectionsAndAdmissionDoNotSubmit) {
    DownloadFixture f;
    status(f.exchange(uploadRequest("GET",contentPath+"?x=1")),400);
    status(f.exchange(uploadRequest("GET",contentPath,"x")),413);
    status(f.exchange(contentRequest("Cookie: a=b\r\n")),400);
    auto request=contentRequest();request.erase(request.find(uploadAuth),uploadAuth.size());status(f.exchange(request),401);
    status(f.exchange(uploadRequest("POST",contentPath)),405);
    f.ready=false;status(f.exchange(contentRequest()),503);f.ready=true;
    for(auto rejection:{SubmitError::QueueFull,SubmitError::Stopping,SubmitError::RateLimited}) {f.dispatcher.rejection=rejection;status(f.exchange(contentRequest()),rejection==SubmitError::RateLimited?429:503);}
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(readFailuresShortReadsAndRejectionAbortWithoutJson) {
    for(int mode=0;mode<4;++mode) {
        DownloadFixture f;Client c(f.server.listenAddress());submitted(f,c);auto g=f.open();if(mode==0)g->rejection=SubmitError::QueueFull;
        if(mode) {f.until([&]{return bool(g->callback);});if(mode==3){g->deliver();f.until([&]{return g->pulls==2;});}
            g->deliver(mode==1||mode==3?std::optional<ErrorCode>(ErrorCode::InternalError):std::nullopt,mode==2);}
        f.until([&]{c.read();return c.eof;});status(c.bytes,200);BOOST_CHECK(body(c.bytes).size()<100000);BOOST_CHECK(c.bytes.find("\"error\"")==std::string::npos);BOOST_CHECK(g->cancelled&&g->destroyed);
        BOOST_CHECK_EQUAL(g->consumedBytes,mode==3?65536:0);
    }
}
BOOST_AUTO_TEST_CASE(runtimeCancellationBeforeStartAndDuringPendingReadIsIdempotent) {
    for(bool immediate:{false,true}) {
        DownloadFixture f;Client c(f.server.listenAddress());submitted(f,c);auto g=f.open();g->immediateCancel=immediate;
        if(!immediate){f.until([&]{return bool(g->callback);});g->cancelFromRuntime();g->cancelFromRuntime();g->deliver();}
        f.until([&]{c.read();return c.eof&&g->destroyed&&(!g->task||g->task->delivered);});BOOST_CHECK_EQUAL(g->consumedBytes,0);BOOST_CHECK_EQUAL(g->pulls,immediate?0:1);BOOST_CHECK(!g->cancellation);
    }
}
BOOST_AUTO_TEST_CASE(timeoutAndForcedStopRetireLateOpenAndReadResults) {
    for(bool timeout:{false,true}) for(bool reading:{false,true}) {
        HttpApiOptions options;options.contentResponseSeconds=0.04;
        DownloadFixture f(options);Client c(f.server.listenAddress());submitted(f,c);f.job()->running=true;
        std::shared_ptr<ReadGate> g;if(reading){g=f.open();f.until([&]{return bool(g->callback);});}
        if(!timeout)f.server.stop(0,[&]{f.stopped=true;});
        f.until([&]{c.read();return c.eof;});auto before=c.bytes;
        if(reading)g->deliver();else g=f.open();
        f.until([&]{return g->destroyed&&(!g->task||g->task->delivered);});c.read();BOOST_CHECK_EQUAL(c.bytes,before);BOOST_CHECK(g->cancelled);BOOST_CHECK_EQUAL(g->consumedBytes,0);
    }
}
BOOST_AUTO_TEST_CASE(disconnectCancelsQueuedOpenAndRunningRead) {
    for(bool reading:{false,true}) {
        HttpApiOptions options;options.contentResponseSeconds=0.04;DownloadFixture f(options);Client c(f.server.listenAddress());submitted(f,c);
        std::shared_ptr<ReadGate> g;if(reading){g=f.open();f.until([&]{return bool(g->callback);});}
        ::shutdown(c.fd,SHUT_RDWR);f.until([&]{return f.finished==1;});
        if(reading){g->deliver();f.until([&]{return g->task->delivered;});BOOST_CHECK(g->cancelled);}else BOOST_CHECK(f.job()->cancelled);
    }
}
BOOST_AUTO_TEST_CASE(acceptedOpenCompletesDuringGraceAndKeepAliveDoesNotReuseConditions) {
    DownloadFixture f;Client c(f.server.listenAddress());submitted(f,c,contentRequest("If-None-Match: "+etag+"\r\n",false,false));auto first=f.open();
    f.until([&]{c.read();return c.bytes.find("\r\n\r\n")!=std::string::npos;});status(c.bytes,304);auto boundary=c.bytes.size();
    submitted(f,c);f.ready=false;f.server.stop(0.5,[&]{f.stopped=true;});auto second=f.open(descriptor(3),1,true);
    f.until([&]{c.read();return c.eof&&f.stopped;});status(c.bytes.substr(boundary),200);BOOST_CHECK_EQUAL(body(c.bytes.substr(boundary)),"abc");BOOST_CHECK_EQUAL(second->consumedBytes,3);
}
BOOST_AUTO_TEST_CASE(realWriterWouldBlockRecoveryAndCancellationHoldReturnedBlock) {
    for(bool cancel:{false,true}) {
        DownloadFixture f({},true);Client c(f.server.listenAddress(),8*1024*1024+65536);submitted(f,c);
        constexpr std::size_t length=8*1024*1024;auto g=f.open(descriptor(length),0,true);
        std::size_t sent=0,pending=0,pulls=0;
        // quit不撤销同批回调，内核也可能继续接收；在同一观察点核对所有状态并触发取消。
        f.until([&] {
            auto s=f.state.lock();
            if(!s || !internal::DownloadStateTestAccess::writeBlocked(*s)) return false;
            pending=internal::DownloadStateTestAccess::pending(*s);
            if(!pending) return false;
            sent=internal::DownloadStateTestAccess::sent(*s);pulls=g->pulls;
            BOOST_CHECK(sent>0&&sent<length);BOOST_CHECK(pending<=65536);
            BOOST_CHECK_EQUAL(sent,g->consumedBytes);BOOST_CHECK_EQUAL(pending,g->unconsumed);
            BOOST_CHECK(!g->callback&&!g->buffer);
            // GateDownload::pull逐次检查unconsumed==0；不能把跨loop迭代计数不变当作无预读证据。
            if(cancel)g->cancelFromRuntime();
            return true;
        });
        std::cout<<"real download WouldBlock: accepted="<<sent<<" lastAccepted="<<(65536-pending)<<" pending="<<pending<<" pulls="<<pulls<<" cancel="<<cancel<<'\n';
        f.until([&]{c.read();return c.eof;});status(c.bytes,200);
        if(cancel){BOOST_CHECK_EQUAL(g->consumedBytes,sent);BOOST_CHECK_EQUAL(g->pulls,pulls);BOOST_CHECK(body(c.bytes).size()<length);}
        else {BOOST_CHECK_EQUAL(body(c.bytes).size(),length);BOOST_CHECK_EQUAL(g->consumedBytes,length);BOOST_CHECK_EQUAL(g->pulls,length/65536);}
        BOOST_CHECK(g->destroyed&&g->cancelled);BOOST_CHECK(!g->cancellation);
    }
}
BOOST_AUTO_TEST_CASE(contentDeadlineIsIndependentOfJsonAndAdmissionSlotReturns) {
    HttpApiOptions options;options.maxInFlight=1;options.responseSeconds=0.01;options.contentResponseSeconds=1;
    DownloadFixture f(options);Client c(f.server.listenAddress());submitted(f,c);
    status(f.exchange(contentRequest()),503);BOOST_CHECK_EQUAL(f.dispatcher.jobs.size(),1);
    // 有限事件定时器控制延后 open，跨过 JSON 期限；没有阻塞 sleep。
    bool completed=false;f.loop.runAfter(0.04,[&]{f.open(descriptor(3),0,true);completed=true;});
    f.until([&]{c.read();return completed&&c.eof;});status(c.bytes,200);BOOST_CHECK_EQUAL(body(c.bytes),"abc");
    Client next(f.server.listenAddress());submitted(f,next);f.open(descriptor(0),1);f.until([&]{next.read();return next.eof;});status(next.bytes,200);
}
BOOST_AUTO_TEST_CASE(invalidDescriptorBeforeStartUsesJsonAndHeadSuppressesBody) {
    for(bool head:{false,true}) {
        DownloadFixture f;Client c(f.server.listenAddress());submitted(f,c,contentRequest({},head));auto value=descriptor();value.name="bad\r\nname";auto g=f.open(value);
        f.until([&]{c.read();return c.eof;});status(c.bytes,500);BOOST_CHECK(body(c.bytes).empty()==head);BOOST_CHECK(c.bytes.find("etag:")==std::string::npos);BOOST_CHECK_EQUAL(g->pulls,0);
    }
}
BOOST_AUTO_TEST_CASE(runtimeCancellationMayReenterFromConsumed) {
    DownloadFixture f;Client c(f.server.listenAddress());submitted(f,c);auto g=f.open();g->cancelOnConsume=true;
    f.until([&]{return bool(g->callback);});g->deliver();f.until([&]{c.read();return c.eof;});
    BOOST_CHECK_EQUAL(g->pulls,1);BOOST_CHECK(g->consumedBytes>0&&g->consumedBytes<=65536);BOOST_CHECK(g->destroyed);BOOST_CHECK(!g->cancellation);
}
