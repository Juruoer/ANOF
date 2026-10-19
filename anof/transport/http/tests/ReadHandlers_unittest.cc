#include <tests/support/EventLoopWait.h>
#define BOOST_TEST_MODULE HttpReads
#include <boost/test/unit_test.hpp>
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/IdentityResponse.h>
#include <muduox/net/EventLoop.h>
#include <nlohmann/json.hpp>
#include "HttpTestSupport.h"
#include "IdentityDispatcherFixture.h"

using namespace anof;
using namespace anof::http;
using namespace anof::http::testing;
using namespace muduox::net;
namespace {
using Json = nlohmann::json;
using Fake = IdentityDispatcherFixture;
using MeJob = Fake::TypedJob<GetMeCommand, UserProfile>;
using SpacesJob = Fake::TypedJob<ListSpacesCommand, std::vector<SpaceView>>;
using NodeJob = Fake::TypedJob<GetNodeCommand, NodeDetails>;
using ChildrenJob = Fake::TypedJob<ListChildrenCommand, Page<NodeDetails, ChildrenCursor>>;
const auto uid = UserId::parse("40000000-0000-4000-8000-000000000001").value();
const auto sid = SpaceId::parse("10000000-0000-4000-8000-000000000001").value();
const auto parent = NodeId::parse("20000000-0000-4000-8000-000000000001").value();
const auto nid = NodeId::parse("30000000-0000-4000-8000-000000000001").value();
const std::string nodePath = "/api/v1/nodes/" + nid.toString();
const std::string childrenPath = "/api/v1/nodes/" + parent.toString() + "/children";
const std::vector<std::string> paths{"/api/v1/users/me", "/api/v1/spaces", nodePath, childrenPath};
const std::string auth = "Authorization: Bearer test-token\r\n";
UserProfile user() {
    return {uid, LoginName::parse("alice").value(), DisplayName::parse("家庭").value(), UserRole::Member, true, AuthEpoch::from(1).value(), Revision::from(9).value(), UtcTime{}, UtcTime{}};
}
NodeDetails node() { return {{nid, sid, parent, "家庭", uid, Revision::from(3).value(), UtcTime{}, UtcTime{}}, DirectoryContent{}}; }
std::vector<SpaceView> spaces() {
    const QuotaDelta limit{ByteCount::from(1099511627776LL).value(), NodeCount::from(100).value()};
    const QuotaDelta used{ByteCount::from(0).value(), NodeCount::from(1).value()};
    const QuotaDelta zero{ByteCount::from(0).value(), NodeCount::from(0).value()};
    const QuotaSnapshot quota{{limit, used, zero}, Revision::from(2).value()};
    return {{{SpaceId::parse("10000000-0000-4000-8000-000000000002").value(), SpaceKind::Private, uid, parent}, quota, std::nullopt},
            {{sid, SpaceKind::Public, std::nullopt, nid}, quota, quota}};
}
std::string request(const std::string& path, const std::string& headers = auth, const std::string& method = "GET", bool close = true) {
    return method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nX-Request-ID: untrusted\r\n" + headers + (close ? "Connection: close\r\n" : "") + "\r\n";
}
void status(const std::string& wire, const char* value) { BOOST_CHECK(wire.find(std::string("HTTP/1.1 ") + value + "\r\n") == 0); }
Json body(const std::string& wire) { return Json::parse(wire.substr(wire.find("\r\n\r\n") + 4)); }
void common(const std::string& wire, RequestId id) {
    BOOST_CHECK(wire.find("x-request-id: " + id.toString() + "\r\n") != std::string::npos);
    BOOST_CHECK(wire.find("cache-control: no-store\r\n") != std::string::npos);
    BOOST_CHECK(wire.find("content-type: application/json\r\n") != std::string::npos);
    BOOST_CHECK(wire.find("untrusted") == std::string::npos);
    BOOST_CHECK(wire.find("set-cookie:") == std::string::npos);
}
class Fixture {
public:
    EventLoop loop;
    Codec codec;
    Identity identity;
    Fake dispatcher{loop};
    bool ready = true;
    std::vector<HttpTraceRecord> finished;
    UnexpectedTransferDispatcher uploadDispatcher;
    HttpApi api;
    HttpServer server{&loop, InetAddress(0, true), "http-read-test"};
    bool stopped = false;
    explicit Fixture(HttpApiOptions options = {})
        : api(codec, identity, [this] { return ready; }, [](auto, const auto&, const auto&) -> HttpStreamCallbacks {
            BOOST_ERROR("Read routed to mkdir"); return {};
        }, dispatcher, uploadDispatcher, {{}, [this](auto record) { finished.push_back(std::move(record)); }}, options) {
        api.configure(server); server.start();
    }
    void until(const std::function<bool()>& done,
               const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        BOOST_REQUIRE(anof::test::waitFor(loop, done, 3, file, line));
    }
    ~Fixture() {
        server.stop(0, [this] { stopped = true; });
        dispatcher.releaseAll();
        until([&] { return stopped && dispatcher.deliveries == dispatcher.jobs.size(); });
    }
    template <typename Job>
    std::shared_ptr<Job> job(std::size_t index = 0) {
        auto value = std::dynamic_pointer_cast<Job>(dispatcher.jobs.at(index));
        BOOST_REQUIRE(value); return value;
    }
    std::string exchange(const std::string& input) {
        Client client(server.listenAddress()); client.send(input);
        until([&] { client.read(); return client.eof; }); return client.bytes;
    }
    template <typename Job, typename Value>
    std::string success(const std::string& input, Value value) {
        const auto index = dispatcher.jobs.size();
        Client client(server.listenAddress()); client.send(input);
        until([&] { return dispatcher.jobs.size() == index + 1; });
        auto task = job<Job>(index); task->result.emplace(std::move(value)); dispatcher.release(task);
        until([&] { client.read(); return client.eof; }); return client.bytes;
    }
};
}  // namespace

BOOST_AUTO_TEST_CASE(meUsesDeferredTypedDispatchAndServerIdentity) {
    Fixture fixture;
    Client client(fixture.server.listenAddress()); client.send(request(paths[0]));
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    client.read(); BOOST_CHECK(client.bytes.empty());
    auto job = fixture.job<MeJob>();
    BOOST_CHECK(job->context.credential.mode == CredentialMode::Bearer);
    BOOST_CHECK(job->context.credential.digest == SessionDigest(SessionDigest::Bytes{}));
    job->result.emplace(user()); fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "200 OK"); common(client.bytes, job->context.requestId);
    BOOST_CHECK_EQUAL(body(client.bytes)["login"].get<std::string>(), "alice");
    BOOST_CHECK_EQUAL(body(client.bytes).size(), 8);
    BOOST_CHECK(client.bytes.find("etag: \"user-" + uid.toString() + "-9\"\r\n") != std::string::npos);
    BOOST_CHECK(!body(client.bytes).contains("auth_epoch"));
    BOOST_REQUIRE_EQUAL(fixture.finished.size(), 1);
    BOOST_CHECK(fixture.finished[0].requestId == job->context.requestId);
}
BOOST_AUTO_TEST_CASE(spacesExposeSortedIndependentQuotas) {
    Fixture fixture;
    const auto wire = fixture.success<SpacesJob>(request(paths[1]), spaces());
    status(wire, "200 OK"); common(wire, fixture.job<SpacesJob>()->context.requestId);
    const auto json = body(wire);
    BOOST_CHECK_EQUAL(json["items"].size(), 2);
    BOOST_CHECK_EQUAL(json["items"][0]["id"].get<std::string>(), sid.toString());
    BOOST_CHECK(json["items"][0]["owner_id"].is_null());
    BOOST_CHECK(json["items"][0]["my_contribution_quota"].is_object());
    BOOST_CHECK_EQUAL(json["items"][1]["owner_id"].get<std::string>(), uid.toString());
    BOOST_CHECK(json["items"][1]["my_contribution_quota"].is_null());
    BOOST_CHECK_EQUAL(json["items"][0]["quota"]["limit"]["bytes"].get<std::string>(), "1099511627776");
    BOOST_CHECK_EQUAL(json["items"][0]["quota"]["revision"].get<std::string>(), "2");
    BOOST_CHECK(wire.find("etag:") == std::string::npos);
}
BOOST_AUTO_TEST_CASE(nodeDirectoryRootAndFileRepresentations) {
    Fixture fixture;
    for (int kind = 0; kind < 3; ++kind) {
        auto value = node();
        if (kind == 1) { value.metadata.parentId.reset(); value.metadata.name.clear(); }
        if (kind == 2) value.content = FileContentMetadata{ByteCount::from(1099511627776LL).value(), {0xab, 0xcd}};
        const auto wire = fixture.success<NodeJob>(request(nodePath), value);
        status(wire, "200 OK"); common(wire, fixture.job<NodeJob>(static_cast<std::size_t>(kind))->context.requestId);
        const auto json = body(wire);
        BOOST_CHECK_EQUAL(json["id"].get<std::string>(), nid.toString());
        BOOST_CHECK(wire.find("etag: " + json["etag"].get<std::string>() + "\r\n") != std::string::npos);
        if (kind == 1) { BOOST_CHECK(json["parent_id"].is_null()); BOOST_CHECK_EQUAL(json["name"].get<std::string>(), ""); }
        if (kind == 2) {
            BOOST_CHECK_EQUAL(json["kind"].get<std::string>(), "file");
            BOOST_CHECK_EQUAL(json["size"].get<std::string>(), "1099511627776");
            BOOST_CHECK_EQUAL(json["sha256"].get<std::string>(), "abcd" + std::string(60, '0'));
        } else BOOST_CHECK(!json.contains("size") && !json.contains("sha256"));
    }
}
BOOST_AUTO_TEST_CASE(childrenBoundCursorRoundTripAndEmptyLastPage) {
    Fixture fixture;
    ChildrenCursor next{uid, sid, parent, {"家庭", nid}};
    Page<NodeDetails, ChildrenCursor> page{{node()}, next};
    const auto wire = fixture.success<ChildrenJob>(request(childrenPath), page);
    status(wire, "200 OK"); common(wire, fixture.job<ChildrenJob>()->context.requestId);
    const auto token = body(wire)["next_cursor"].get<std::string>();
    const auto decoded = std::get<ChildrenCursor>(decodeChildrenCursor(token));
    BOOST_CHECK(decoded.userId == uid && decoded.spaceId == sid && decoded.parent == parent);
    BOOST_CHECK_EQUAL(body(wire)["items"][0]["etag"].get<std::string>(), "\"node-" + nid.toString() + "-3\"");
    Client client(fixture.server.listenAddress()); client.send(request(childrenPath + "?limit=1&cursor=" + token));
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 2; });
    auto job = fixture.job<ChildrenJob>(1);
    BOOST_CHECK_EQUAL(job->command->size.value(), 1);
    BOOST_CHECK(job->command->parent == parent);
    BOOST_REQUIRE(job->command->after);
    BOOST_CHECK_EQUAL(job->command->after->position.name, "家庭");
    job->result.emplace(Page<NodeDetails, ChildrenCursor>{}); fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "200 OK");
    BOOST_CHECK(body(client.bytes)["items"].empty());
    BOOST_CHECK(body(client.bytes)["next_cursor"].is_null());
}
BOOST_AUTO_TEST_CASE(childrenBindingIsPassedToApplicationBeforeBusinessErrors) {
    Fixture fixture;
    const ChildrenCursor different{UserId::parse("40000000-0000-4000-8000-000000000002").value(), sid, nid, {"x", nid}};
    const auto cursor = encodeCursor(different).value();
    const std::pair<ErrorCode, const char*> cases[]{{ErrorCode::Unauthenticated, "401 Unauthorized"}, {ErrorCode::NotFound, "404 Not Found"}, {ErrorCode::InvalidArgument, "400 Bad Request"}, {ErrorCode::ParentNotDirectory, "409 Conflict"}};
    for (const auto& item : cases) {
        const auto index = fixture.dispatcher.jobs.size();
        Client client(fixture.server.listenAddress()); client.send(request(childrenPath + "?cursor=" + cursor));
        fixture.until([&] { return fixture.dispatcher.jobs.size() == index + 1; });
        auto job = fixture.job<ChildrenJob>(index);
        BOOST_REQUIRE(job->command->after);
        BOOST_CHECK(job->command->after->userId == different.userId);
        BOOST_CHECK(job->command->after->parent != job->command->parent);
        job->result.emplace(item.first); fixture.dispatcher.release(job);
        fixture.until([&] { client.read(); return client.eof; });
        status(client.bytes, item.second);
        BOOST_CHECK(body(client.bytes)["error"]["details"].empty());
        if (item.first == ErrorCode::ParentNotDirectory) BOOST_CHECK_EQUAL(body(client.bytes)["error"]["code"].get<std::string>(), "PARENT_NOT_DIRECTORY");
    }
}
BOOST_AUTO_TEST_CASE(allReadsRejectMissingMalformedAndCookieCredentials) {
    Fixture fixture;
    for (const auto& path : paths) {
        for (const auto& headers : {std::string{}, std::string("Authorization: Bearer bad\r\n")}) {
            const auto wire = fixture.exchange(request(path, headers));
            status(wire, "401 Unauthorized");
            BOOST_CHECK(wire.find("www-authenticate: Bearer realm=\"anof\"") != std::string::npos);
        }
        status(fixture.exchange(request(path, auth + "Cookie:\r\n")), "400 Bad Request");
        status(fixture.exchange(request(path, auth + auth)), "400 Bad Request");
    }
    status(fixture.exchange(request("/api/v1/nodes/bad/children?limit=bad", "")), "401 Unauthorized");
    BOOST_CHECK(fixture.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(noBodyFramingRejectsBeforeContinueOrSubmission) {
    Fixture fixture;
    for (const auto& path : paths) {
        for (const auto& framing : {"Content-Length: 1\r\n", "Transfer-Encoding: chunked\r\n", "Content-Length: 0\r\nTrailer: X-Test\r\n"}) {
            const auto wire = fixture.exchange(request(path, auth + framing + "Expect: 100-continue\r\n"));
            // 某些 Expect/定界组合可先被框架拒绝；应用处理的非零正文返回 400。
            BOOST_CHECK(wire.find("HTTP/1.1 4") == 0);
            BOOST_CHECK(wire.find("100 Continue") == std::string::npos);
        }
    }
    BOOST_CHECK(fixture.dispatcher.jobs.empty());
    const auto wire = fixture.success<MeJob>(request(paths[0], auth + "Content-Length: 0\r\n"), user());
    status(wire, "200 OK");
}
BOOST_AUTO_TEST_CASE(pathAndQueryValidationNeverDispatchesInvalidCommands) {
    Fixture fixture;
    for (const auto& path : {std::string("/api/v1/nodes/bad"), std::string("/api/v1/nodes/00000000-0000-0000-0000-000000000000"), std::string("/api/v1/nodes/bad/children"), paths[0] + "?limit=1", paths[1] + "?cursor=x", nodePath + "?unknown=1"}) status(fixture.exchange(request(path)), "400 Bad Request");
    for (const auto& query : {"limit=0", "limit=201", "limit=01", "limit=+1", "limit=1.0", "limit=1&limit=2", "limit=1&%6cimit=2", "cursor=", "cursor=bad", "cursor=x&cursor=x", "unknown=1"}) status(fixture.exchange(request(childrenPath + "?" + query)), "400 Bad Request");
    BOOST_CHECK(fixture.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(readMethodsRemainExplicitAndDoNotEnableConditionalGet) {
    Fixture fixture;
    for (const auto& path : paths) for (const auto& method : {"HEAD", "OPTIONS", "POST", "DELETE"}) {
        const auto wire = fixture.exchange(request(path, auth, method));
        status(wire, "405 Method Not Allowed");
        BOOST_CHECK(wire.find(path == paths[0] ? "allow: GET, PATCH\r\n" : "allow: GET\r\n") != std::string::npos);
        BOOST_CHECK(wire.find("access-control-") == std::string::npos);
    }
    BOOST_CHECK(fixture.dispatcher.jobs.empty());
    const auto wire = fixture.success<NodeJob>(request(nodePath, auth + "If-None-Match: *\r\n"), node());
    status(wire, "200 OK");
}
BOOST_AUTO_TEST_CASE(allReadRoutesMapSubmissionRejectionsWithoutCompletion) {
    Fixture fixture;
    for (const auto& path : paths) for (const auto& item : {std::pair{SubmitError::RateLimited, "429 Too Many Requests"}, std::pair{SubmitError::QueueFull, "503 Service Unavailable"}, std::pair{SubmitError::Stopping, "503 Service Unavailable"}, std::pair{SubmitError::InternalError, "500 Internal Server Error"}}) {
        fixture.dispatcher.rejection = item.first;
        const auto wire = fixture.exchange(request(path)); status(wire, item.second);
        BOOST_CHECK_EQUAL(body(wire)["error"]["code"].get<std::string>(), item.first == SubmitError::RateLimited ? "RATE_LIMITED" : item.first == SubmitError::InternalError ? "INTERNAL_ERROR" : "TEMPORARILY_UNAVAILABLE");
    }
    BOOST_CHECK(fixture.dispatcher.jobs.empty());
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);
}
BOOST_AUTO_TEST_CASE(nodeNotFoundAndInvalidOutputRemainErrors) {
    Fixture fixture;
    const auto missing = fixture.success<NodeJob>(request(nodePath), Result<NodeDetails>(ErrorCode::NotFound));
    status(missing, "404 Not Found");
    BOOST_CHECK_EQUAL(body(missing)["error"]["code"].get<std::string>(), "NOT_FOUND");
    auto malformed = node(); malformed.metadata.parentId.reset();
    const auto bad = fixture.success<NodeJob>(request(nodePath), malformed);
    status(bad, "500 Internal Server Error");
    BOOST_CHECK(!body(bad).contains("id"));
}
BOOST_AUTO_TEST_CASE(realChildrenPageExceedsSingleWriteWithoutTruncation) {
    Fixture fixture;
    Page<NodeDetails, ChildrenCursor> page;
    for (unsigned i = 0; i < 200; ++i) {
        auto value = node(); char id[37];
        std::snprintf(id, sizeof id, "30000000-0000-4000-8000-%012x", i + 1);
        value.metadata.id = NodeId::parse(id).value();
        value.metadata.name = std::string(245, 'x') + std::to_string(1000 + i);
        page.items.push_back(std::move(value));
    }
    const auto& last = page.items.back().metadata;
    page.next = ChildrenCursor{uid, sid, parent, {last.name, last.id}};
    const auto wire = fixture.success<ChildrenJob>(request(childrenPath + "?limit=200"), page);
    status(wire, "200 OK");
    BOOST_CHECK(wire.size() > 64 * 1024);
    const auto json = body(wire);
    BOOST_CHECK_EQUAL(json["items"].size(), 200);
    BOOST_CHECK_EQUAL(json["items"][199]["id"].get<std::string>(), last.id.toString());
    const auto cursor = std::get<ChildrenCursor>(decodeChildrenCursor(json["next_cursor"].get<std::string>()));
    BOOST_CHECK(cursor.position.id == last.id);
}
BOOST_AUTO_TEST_CASE(readTimeoutCancellationAndLateResultDoNotAffectNextRequest) {
    Fixture fixture({128, 15, 0.03});
    Client first(fixture.server.listenAddress()); first.send(request(paths[0]));
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<MeJob>();
    fixture.until([&] { first.read(); return first.eof; });
    status(first.bytes, "500 Internal Server Error");
    BOOST_CHECK(job->cancelled);
    Client next(fixture.server.listenAddress()); next.send(request(nodePath));
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 2; });
    fixture.dispatcher.release(job);
    auto active = fixture.job<NodeJob>(1); active->result.emplace(node()); fixture.dispatcher.release(active);
    fixture.until([&] { next.read(); return next.eof; });
    status(next.bytes, "200 OK");
    common(next.bytes, active->context.requestId);
    BOOST_CHECK(next.bytes.find(job->context.requestId.toString()) == std::string::npos);
}
BOOST_AUTO_TEST_CASE(readsShareAdmissionAndKeepAliveIdentityWithHealth) {
    Fixture fixture({1, 15, 30});
    Client client(fixture.server.listenAddress()); client.send(request(paths[0], auth, "GET", false));
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    status(fixture.exchange(request(paths[1])), "503 Service Unavailable");
    auto job = fixture.job<MeJob>(); job->result.emplace(user()); fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return !fixture.finished.empty() && fixture.dispatcher.deliveries == 1; });
    client.send(request("/health/live", ""));
    fixture.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(client.bytes.find("\"status\":\"live\"") != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 1);
    BOOST_CHECK(fixture.finished.back().requestId != job->context.requestId);
    fixture.ready = false;
    for (const auto& path : paths) status(fixture.exchange(request(path)), "503 Service Unavailable");
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 1);
}

BOOST_AUTO_TEST_CASE(ownedIdsAndPageLimitsReachTypedCommands) {
    Fixture fixture;
    const std::string upper = "AAAAAAAA-0000-4000-8000-000000000001";
    Client nodeClient(fixture.server.listenAddress());
    auto text = request("/api/v1/nodes/" + upper);
    nodeClient.send(text); text.assign(text.size(), 'x');
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto nodeJob = fixture.job<NodeJob>();
    BOOST_CHECK_EQUAL(nodeJob->command->id.toString(), "aaaaaaaa-0000-4000-8000-000000000001");
    nodeJob->result.emplace(ErrorCode::NotFound); fixture.dispatcher.release(nodeJob);
    fixture.until([&] { nodeClient.read(); return nodeClient.eof; });
    for (const auto& item : {std::pair{"", 100}, std::pair{"?limit=200", 200}}) {
        const auto index = fixture.dispatcher.jobs.size();
        Client client(fixture.server.listenAddress()); client.send(request(childrenPath + item.first));
        fixture.until([&] { return fixture.dispatcher.jobs.size() == index + 1; });
        auto job = fixture.job<ChildrenJob>(index);
        BOOST_CHECK_EQUAL(job->command->size.value(), item.second);
        BOOST_CHECK(job->command->parent == parent);
        BOOST_CHECK(!job->command->after);
        job->result.emplace(Page<NodeDetails, ChildrenCursor>{}); fixture.dispatcher.release(job);
        fixture.until([&] { client.read(); return client.eof; });
        status(client.bytes, "200 OK");
    }
}
