#include <tests/support/EventLoopWait.h>
#define BOOST_TEST_MODULE HttpApi
#include <anof/transport/http/HttpApi.h>
#include <muduox/net/EventLoop.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

#include "HttpTestSupport.h"

using namespace anof;
using namespace anof::http;
using namespace muduox::net;
namespace {
using namespace anof::http::testing;
class Fixture {
public:
    EventLoop loop;
    Codec codec;
    Identity identity;
    UnexpectedIdentityDispatcher identityDispatcher;
    bool ready = true;
    int entered = 0;
    int cancelled = 0;
    std::optional<RequestContext> context;
    std::vector<HttpTraceRecord> committed;
    std::vector<HttpTraceRecord> finished;
    UnexpectedTransferDispatcher uploadDispatcher;
    UnexpectedDriveDispatcher driveDispatcher;
    HttpApi api;
    HttpServer server;
    explicit Fixture(std::size_t limit = 128)
        : api(codec, identity, [this] { return ready; }, [this](RequestContext owned, const HttpRequest&, const auto&) {
                  ++entered;
                  context = owned;
                  HttpStreamCallbacks callbacks;
                  callbacks.body = [](auto part) { return part.size(); };
                  callbacks.cancel = [this] { ++cancelled; };
                  return callbacks; }, identityDispatcher, uploadDispatcher, driveDispatcher, developmentConfiguration(), { [this](auto record) { committed.push_back(std::move(record)); }, [this](auto record) { finished.push_back(std::move(record)); } }, { limit, 15, 30 }), server(&loop, InetAddress(0, true), "anof-http-test") {
        api.configure(server);
        server.start();
    }
    ~Fixture() {
        bool stopped = false;
        server.stop(0, [&] { stopped = true; });
        if (!anof::test::waitFor(loop, [&] { return stopped; }, 3)) std::terminate();
    }
    void until(const std::function<bool()>& done,
               const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        BOOST_REQUIRE(anof::test::waitFor(loop, done, 3, file, line));
    }
    std::string exchange(const std::string& request) {
        Client client(server.listenAddress());
        client.send(request);
        until([&] { client.read(); return client.eof; });
        return client.bytes;
    }
};
std::string get(std::string path, std::string method = "GET") {
    return method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nX-Request-ID: untrusted\r\n\r\n";
}
std::string post(std::string headers) {
    return "POST /api/v1/nodes HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\nExpect: 100-continue\r\n" + headers + "\r\n";
}
nlohmann::json jsonBody(const std::string& response) {
    return nlohmann::json::parse(response.substr(response.find("\r\n\r\n") + 4));
}
void status(const std::string& response, const std::string& line) {
    BOOST_CHECK(response.find("HTTP/1.1 " + line + "\r\n") == 0);
}
}  // namespace

BOOST_AUTO_TEST_CASE(healthAndServerIdentity) {
    Fixture fixture;
    auto live = fixture.exchange(get("/health/live?token=secret"));
    status(live, "200 OK");
    BOOST_REQUIRE(!fixture.finished.empty());
    BOOST_CHECK_EQUAL(fixture.finished.back().route, "/health/live");
    BOOST_CHECK_EQUAL(jsonBody(live)["status"].get<std::string>(), "live");
    BOOST_REQUIRE_EQUAL(fixture.finished.size(), 1);
    const auto id = fixture.finished.back().requestId.toString();
    BOOST_CHECK(live.find("x-request-id: " + id + "\r\n") != std::string::npos);
    BOOST_CHECK(live.find("untrusted") == std::string::npos);
    BOOST_CHECK(live.find("cache-control: no-store\r\n") != std::string::npos);
    fixture.ready = false;
    auto unavailable = fixture.exchange(get("/health/ready"));
    status(unavailable, "503 Service Unavailable");
    BOOST_CHECK_EQUAL(jsonBody(unavailable)["error"]["request_id"].get<std::string>(), fixture.finished.back().requestId.toString());
    status(fixture.exchange(get("/health/live")), "200 OK");
    fixture.ready = true;
    status(fixture.exchange(get("/health/ready")), "200 OK");
    BOOST_CHECK_EQUAL(fixture.codec.calls, 0);
    BOOST_CHECK_EQUAL(fixture.entered, 0);
    BOOST_CHECK_EQUAL(fixture.committed.size(), 4);
    BOOST_CHECK_EQUAL(fixture.finished.size(), 4);
}
BOOST_AUTO_TEST_CASE(routingAndMethodPolicy) {
    Fixture fixture;
    status(fixture.exchange(get("/unknown")), "404 Not Found");
    for (const auto& method : { "HEAD", "OPTIONS", "POST" }) {
        const auto response = fixture.exchange(get("/health/live", method));
        status(response, "405 Method Not Allowed");
        BOOST_CHECK(response.find("allow: GET\r\n") != std::string::npos);
        BOOST_CHECK(response.find("access-control-") == std::string::npos);
        if (std::string(method) == "HEAD") BOOST_CHECK(response.substr(response.find("\r\n\r\n") + 4).empty());
    }
    auto response = fixture.exchange(get("/api/v1/nodes"));
    status(response, "405 Method Not Allowed");
    BOOST_CHECK(response.find("allow: POST\r\n") != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.entered, 0);
}
BOOST_AUTO_TEST_CASE(headRejectionDoesNotWaitForBodyOrSendContinue) {
    Fixture fixture;
    const std::pair<std::string, std::string> cases[] = {
        { "Content-Type: application/json\r\n", "401 Unauthorized" },
        { "Authorization: Bearer bad\r\nContent-Type: application/json\r\n", "401 Unauthorized" },
        { "Authorization: Bearer test-token\r\nCookie: __Host-anof_session=x\r\n", "400 Bad Request" },
        { "Authorization: Bearer test-token\r\nAuthorization: Bearer test-token\r\n", "400 Bad Request" },
        { "Authorization: Bearer test-token\r\nContent-Type: text/plain\r\n", "415 Unsupported Media Type" }
    };
    for (const auto& entry : cases) {
        const auto response = fixture.exchange(post(entry.first));
        status(response, entry.second);
        BOOST_CHECK(response.find("100 Continue") == std::string::npos);
        BOOST_CHECK(response.find("Connection: close") != std::string::npos);
        // 重复 Authorization 可在 muduox 头部校验中先被拒绝，框架错误允许空正文。
        if (fixture.finished.back().source == HttpResponseSource::Application) {
            BOOST_CHECK_EQUAL(jsonBody(response)["error"]["request_id"].get<std::string>(), fixture.finished.back().requestId.toString());
        } else {
            BOOST_CHECK(response.substr(response.find("\r\n\r\n") + 4).empty());
        }
        if (entry.second == "401 Unauthorized") BOOST_CHECK(response.find("www-authenticate: Bearer realm=\"anof\"") != std::string::npos);
    }
    fixture.ready = false;
    status(fixture.exchange(post("Authorization: Bearer test-token\r\nContent-Type: application/json\r\n")), "503 Service Unavailable");
    BOOST_CHECK_EQUAL(fixture.entered, 0);
}
BOOST_AUTO_TEST_CASE(admissionRetainedUntilExchangeEnds) {
    Fixture fixture(1);
    Client pending(fixture.server.listenAddress());
    pending.send(post("Authorization: Bearer test-token\r\nContent-Type: application/json\r\n"));
    fixture.until([&] { pending.read(); return fixture.entered == 1 && pending.bytes.find("100 Continue") != std::string::npos; });
    BOOST_CHECK(fixture.finished.empty());
    BOOST_REQUIRE(fixture.context);
    BOOST_CHECK(fixture.context->credential.mode == CredentialMode::Bearer);
    BOOST_CHECK(fixture.context->credential.digest == SessionDigest(SessionDigest::Bytes{}));
    const auto denied = fixture.exchange(get("/health/live"));
    status(denied, "503 Service Unavailable");
    BOOST_CHECK(fixture.ready);
    ::shutdown(pending.fd, SHUT_RDWR);
    fixture.until([&] { return fixture.cancelled == 1; });
    status(fixture.exchange(get("/health/live")), "200 OK");
    BOOST_CHECK_EQUAL(fixture.entered, 1);
}
BOOST_AUTO_TEST_CASE(protocolErrorsAreObservedWithoutJsonEnvelope) {
    Fixture fixture;
    const auto response = fixture.exchange("GET /health/live HTTP/1.1\r\nHost: localhost\r\nContent-Length: invalid\r\n\r\n");
    status(response, "400 Bad Request");
    BOOST_CHECK(response.substr(response.find("\r\n\r\n") + 4).empty());
    BOOST_REQUIRE_EQUAL(fixture.finished.size(), 1);
    BOOST_CHECK(fixture.finished[0].source == HttpResponseSource::HttpError);
    BOOST_CHECK(fixture.finished[0].outcome == HttpExchangeOutcome::Failed);
    BOOST_CHECK_EQUAL(*fixture.finished[0].status, 400);
    BOOST_CHECK(response.find("x-request-id: " + fixture.finished[0].requestId.toString()) != std::string::npos);
}
BOOST_AUTO_TEST_CASE(configurationIsValidatedAndInstalledOnce) {
    Fixture fixture;
    BOOST_CHECK_THROW(fixture.api.configure(fixture.server), std::logic_error);
    BOOST_CHECK_THROW(HttpApi(fixture.codec, fixture.identity, {}, [](auto, const auto&, const auto&) { return HttpStreamCallbacks{}; }, fixture.identityDispatcher, fixture.uploadDispatcher, fixture.driveDispatcher, developmentConfiguration()), std::invalid_argument);
    BOOST_CHECK_THROW(HttpApi(fixture.codec, fixture.identity, [] { return true; }, {}, fixture.identityDispatcher, fixture.uploadDispatcher, fixture.driveDispatcher, developmentConfiguration()), std::invalid_argument);
    BOOST_CHECK_THROW(HttpApi(fixture.codec, fixture.identity, [] { return true; }, [](auto, const auto&, const auto&) { return HttpStreamCallbacks{}; }, fixture.identityDispatcher, fixture.uploadDispatcher, fixture.driveDispatcher, developmentConfiguration(), {}, { 129, 15, 30 }), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(keepAliveIdsArePerExchange) {
    Fixture fixture;
    const auto response = fixture.exchange("GET /health/live HTTP/1.1\r\nHost: localhost\r\n\r\n" + get("/health/ready"));
    BOOST_REQUIRE_EQUAL(fixture.finished.size(), 2);
    const auto& first = fixture.finished[0];
    const auto& second = fixture.finished[1];
    BOOST_CHECK_EQUAL(first.exchange.connection, second.exchange.connection);
    BOOST_CHECK(first.exchange.request != second.exchange.request);
    BOOST_CHECK(first.requestId != second.requestId);
    BOOST_CHECK(response.find("x-request-id: " + first.requestId.toString()) != std::string::npos);
    BOOST_CHECK(response.find("x-request-id: " + second.requestId.toString()) != std::string::npos);
    BOOST_CHECK(first.outcome == HttpExchangeOutcome::Completed);
    BOOST_CHECK(second.outcome == HttpExchangeOutcome::Completed);
}
BOOST_AUTO_TEST_CASE(routeBodyLimitSurvivesEndpointBinding) {
    Fixture fixture;
    const auto response = fixture.exchange("POST /api/v1/nodes HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer test-token\r\nContent-Type: application/json\r\nContent-Length: 65537\r\nExpect: 100-continue\r\n\r\n");
    status(response, "413 Content Too Large");
    BOOST_CHECK(response.find("100 Continue") == std::string::npos);
    BOOST_CHECK_EQUAL(fixture.entered, 0);
    BOOST_REQUIRE_EQUAL(fixture.finished.size(), 1);
    BOOST_CHECK_EQUAL(*fixture.finished[0].status, 413);
}

BOOST_AUTO_TEST_CASE(trace_routes_are_static_templates_without_identifiers_or_queries) {
    Fixture f;
    f.exchange(get("/api/v1/nodes/secret-node?token=secret-query"));
    BOOST_REQUIRE(!f.finished.empty());
    BOOST_CHECK_EQUAL(f.finished.back().route, "/api/v1/nodes/:node_id");
    f.exchange(get("/unknown-secret-path?token=secret-query"));
    BOOST_CHECK_EQUAL(f.finished.back().route, "unmatched");
}
