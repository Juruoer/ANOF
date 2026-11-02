#define BOOST_TEST_MODULE DownloadProtocol
#include <muduox/net/Buffer.h>

#include "DownloadTestSupport.h"
using namespace anof;
using namespace anof::http;
using namespace anof::http::testing;
namespace {
const auto rid = Identity{}.forExchange(1, 1);
const std::string etag = "\"sha256-" + std::string(64, 'a') + "\"";
muduox::net::HttpHeaders headers(std::string name, std::string value) {
    muduox::net::HttpHeaders h;
    h.append(name, value);
    return h;
}
int code(const Result<DownloadResponse>& r) {
    BOOST_REQUIRE(r);
    return static_cast<int>(r.value().headers.statusCode());
}
}  // namespace
BOOST_AUTO_TEST_CASE(conditionPriorityAndDuplicateSemantics) {
    for (bool head : { false, true }) {
        BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), headers("If-None-Match", etag), head, rid)), 304);
        BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), headers("If-None-Match", "W/" + etag), head, rid)), 304);
        BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), headers("If-Match", "*"), head, rid)), 200);
        for (auto value : { std::string("\"other\""), "W/" + etag }) {
            auto r = downloadResponse(descriptor(), headers("If-Match", value), head, rid);
            BOOST_REQUIRE(!r);
            BOOST_CHECK(r.error().code == ErrorCode::PreconditionFailed);
        }
        auto invalid = downloadResponse(descriptor(), headers("If-None-Match", "invalid"), head, rid);
        BOOST_REQUIRE(!invalid);
        BOOST_CHECK(invalid.error().code == ErrorCode::InvalidArgument);
        auto h = headers("If-Match", "\"other\"");
        h.append("If-None-Match", "invalid");
        auto r = downloadResponse(descriptor(), h, head, rid);
        BOOST_REQUIRE(!r);
        BOOST_CHECK(r.error().code == ErrorCode::PreconditionFailed);
        h = headers("If-Match", "\"other\"");
        h.append("If-Match", etag);
        BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), h, head, rid)), 200);
    }
}
BOOST_AUTO_TEST_CASE(singleRangeResolutionAndIgnoredCases) {
    for (auto value : { "bytes=2-4", "bytes=-3", "bytes=99997-" }) {
        auto r = downloadResponse(descriptor(), headers("Range", value), false, rid);
        BOOST_CHECK_EQUAL(code(r), 206);
        BOOST_CHECK_EQUAL(*r.value().length, 3);
    }
    for (auto value : { "bytes=5-2", "bytes=0-1,3-4", "items=0-1", "garbage" }) BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), headers("Range", value), false, rid)), 200);
    auto h = headers("Range", "bytes=100000-");
    auto r = downloadResponse(descriptor(), h, false, rid);
    BOOST_CHECK_EQUAL(code(r), 416);
    BOOST_CHECK_EQUAL(*r.value().length, 0);
    BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), h, true, rid)), 200);
    BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(0), h, false, rid)), 200);
    h.append("If-Range", "\"other\"");
    BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), h, false, rid)), 200);
    h.set("If-Range", etag);
    BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), h, false, rid)), 416);
    h.append("If-Range", etag);
    BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), h, false, rid)), 200);
    h = headers("Range", "bytes=2-4");
    h.append("Range", "bytes=6-8");
    BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), h, false, rid)), 200);
}
BOOST_AUTO_TEST_CASE(dateConditionsIgnoredAndOrdinaryConditionsPrecedeRange) {
    auto h = headers("If-Modified-Since", "Sun, 06 Nov 2094 08:49:37 GMT");
    h.append("If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:37 GMT");
    BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), h, false, rid)), 200);
    h.append("Range", "bytes=0-1");
    h.append("If-Range", "Sun, 06 Nov 1994 08:49:37 GMT");
    BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), h, false, rid)), 200);
    h.append("If-None-Match", etag);
    BOOST_CHECK_EQUAL(code(downloadResponse(descriptor(), h, false, rid)), 304);
}
BOOST_AUTO_TEST_CASE(headerSuppressionSafeFilenameAndSyntheticLargeLength) {
    auto render = [](const DownloadResponse& r, bool head) {muduox::net::Buffer b;BOOST_REQUIRE(r.headers.appendStreamHeaders(&b,HttpRequest::kHttp11,head?HttpRequest::kHead:HttpRequest::kGet,r.length));return b.retrieveAllAsString(); };
    auto r = downloadResponse(descriptor(), {}, true, rid);
    BOOST_REQUIRE(r);
    auto wire = render(r.value(), true);
    BOOST_CHECK(wire.find("Content-Length: 100000") != std::string::npos);
    BOOST_CHECK(wire.find("filename=\"download\"; filename*=UTF-8''%") != std::string::npos);
    BOOST_CHECK(wire.find("家庭") == std::string::npos);
    BOOST_CHECK(wire.find("last-modified") == std::string::npos);
    r = downloadResponse(descriptor(), headers("If-None-Match", etag), false, rid);
    BOOST_REQUIRE(r);
    wire = render(r.value(), false);
    for (auto field : { "Content-Length:", "content-type:", "content-disposition:" }) BOOST_CHECK(wire.find(field) == std::string::npos);
    auto large = downloadResponse(descriptor(kMaxFileBytes), {}, true, rid);
    BOOST_REQUIRE(large);
    BOOST_CHECK_EQUAL(*large.value().length, static_cast<std::size_t>(kMaxFileBytes));
    auto bad = descriptor();
    bad.name = "bad\r\nname";
    BOOST_CHECK(!downloadResponse(bad, {}, false, rid));
}
BOOST_AUTO_TEST_CASE(conditionCopyOwnsOnlyBoundedNeededHeaders) {
    auto h = headers("If-Match", etag);
    h.append("Authorization", "secret");
    h.append("Range", "bytes=0-1");
    auto copy = copyDownloadConditions(h);
    h.clear();
    BOOST_REQUIRE(std::holds_alternative<muduox::net::HttpHeaders>(copy));
    const auto& owned = std::get<muduox::net::HttpHeaders>(copy);
    BOOST_CHECK_EQUAL(owned.size(), 2);
    BOOST_CHECK_EQUAL(owned.getSingle("if-match").value, etag);
    h.append("If-Match", std::string(65537, 'x'));
    BOOST_CHECK(std::holds_alternative<ProtocolError>(copyDownloadConditions(h)));
}
