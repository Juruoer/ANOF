#include <tests/support/EventLoopWait.h>
#define BOOST_TEST_MODULE HttpIdentityHandlers
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
using RegisterJob = Fake::TypedJob<RegisterCommand, UserProfile, RequestId>;
using LoginJob = Fake::TypedJob<LoginCommand, LoginResult, RequestId>;
using LogoutJob = Fake::TypedJob<LogoutCommand, void>;
using CreateJob = Fake::TypedJob<CreateInviteCommand, CreatedInvite>;
using ListJob = Fake::TypedJob<ListInvitesCommand, Page<InviteInfo, InviteCursor>>;
using RevokeJob = Fake::TypedJob<RevokeInviteCommand, void>;
const auto uid = UserId::parse("40000000-0000-4000-8000-000000000001").value();
const auto iid = InviteId::parse("50000000-0000-4000-8000-000000000001").value();
const std::string token(43, 'A');
const std::string inviteToken = "anof_invite_" + token;
const std::string auth = "Authorization: Bearer test-token\r\n";
const std::string registration = Json{{"login", "Alice"}, {"display_name", "家庭"}, {"password", "  password  "}, {"invite_token", inviteToken}}.dump();
const std::string login = R"({"login":"Alice","password":"  password  ","credential_mode":"bearer"})";
const std::string registerPath = "/api/v1/auth/register";
const std::string loginPath = "/api/v1/auth/sessions";
const std::string logoutPath = "/api/v1/auth/session";
const std::string invitesPath = "/api/v1/admin/invites";
const std::string revokePath = invitesPath + "/" + iid.toString();
UserProfile user() {
    return {uid, LoginName::parse("alice").value(), DisplayName::parse("家庭").value(), UserRole::Member, true, AuthEpoch::from(1).value(), Revision::from(1).value(), UtcTime{}, UtcTime{}};
}
InviteInfo invite() { return {iid, uid, UtcTime{}, UtcTime{} + std::chrono::hours(24), InviteUseLimit::from(2).value(), InviteUseCount::from(0).value(), std::nullopt}; }
LoginResult loggedIn() { return {SecretToken::session(token).value(), UtcTime{} + std::chrono::hours(168), user()}; }
CreatedInvite created() { return {invite(), SecretToken::invite(inviteToken).value()}; }
std::string request(const std::string& method, const std::string& path, const std::string& headers = auth, const std::optional<std::string>& data = std::nullopt) {
    return method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nX-Request-ID: untrusted\r\nConnection: close\r\n" + headers +
        (data ? "Content-Type: application/json\r\nContent-Length: " + std::to_string(data->size()) + "\r\n" : "") + "\r\n" + data.value_or("");
}
void status(const std::string& wire, const char* value) { BOOST_CHECK_MESSAGE(wire.find(std::string("HTTP/1.1 ") + value + "\r\n") == 0, "Expected " << value << ", received " << wire.substr(0, wire.find("\r\n"))); }
Json body(const std::string& wire) { return Json::parse(wire.substr(wire.find("\r\n\r\n") + 4)); }
void common(const std::string& wire, RequestId id) {
    BOOST_CHECK(wire.find("x-request-id: " + id.toString() + "\r\n") != std::string::npos);
    BOOST_CHECK(wire.find("cache-control: no-store\r\n") != std::string::npos);
    BOOST_CHECK(wire.find("untrusted") == std::string::npos);
    BOOST_CHECK(wire.find("set-cookie:") == std::string::npos);
    BOOST_CHECK(wire.find("location:") == std::string::npos);
}
void noSecret(const std::string& wire) {
    BOOST_CHECK(wire.find(token) == std::string::npos);
    BOOST_CHECK(wire.find("password") == std::string::npos);
    BOOST_CHECK(wire.find("test-token") == std::string::npos);
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
    HttpServer server{&loop, InetAddress(0, true), "http-identity-test"};
    bool stopped = false;
    explicit Fixture(HttpApiOptions options = {})
        : api(codec, identity, [this] { return ready; }, [](auto, const auto&, const auto&) -> HttpStreamCallbacks {
            BOOST_ERROR("Identity routed to mkdir"); return {};
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

BOOST_AUTO_TEST_CASE(registerUsesAnonymousContextTrustedPeerAndCreatesNoSession) {
    Fixture f;
    Client client(f.server.listenAddress());
    client.send(request("POST", registerPath, "Forwarded: for=203.0.113.9\r\nX-Forwarded-For: 198.51.100.2\r\nX-Real-IP: ::1\r\n", registration));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    client.read(); BOOST_CHECK(client.bytes.empty());
    auto job = f.job<RegisterJob>();
    BOOST_CHECK(job->command->source == AuthenticationSource::ipv4({127, 0, 0, 1}));
    BOOST_CHECK_EQUAL(job->command->password.view(), "  password  ");
    BOOST_CHECK_EQUAL(job->command->invite.view(), inviteToken);
    job->result.emplace(user()); f.dispatcher.release(job);
    f.until([&] { client.read(); return client.eof; });
    status(client.bytes, "201 Created"); common(client.bytes, job->context); noSecret(client.bytes);
    BOOST_CHECK_EQUAL(body(client.bytes)["login"].get<std::string>(), "alice");
    BOOST_CHECK_EQUAL(body(client.bytes).size(), 8);
    BOOST_CHECK_EQUAL(f.codec.calls, 0);
    BOOST_CHECK_EQUAL(f.dispatcher.submissions, 1);
}
BOOST_AUTO_TEST_CASE(loginReturnsBearerSecretOnlyAfterCompletion) {
    Fixture f;
    Client client(f.server.listenAddress()); client.send(request("POST", loginPath, "X-Forwarded-For: 203.0.113.1\r\n", login));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    auto job = f.job<LoginJob>();
    BOOST_CHECK(job->command->source == AuthenticationSource::ipv4({127, 0, 0, 1}));
    BOOST_CHECK(job->command->mode == CredentialMode::Bearer);
    BOOST_CHECK_EQUAL(job->command->password.view(), "  password  ");
    client.read(); BOOST_CHECK(client.bytes.empty());
    job->result.emplace(loggedIn()); f.dispatcher.release(job);
    f.until([&] { client.read(); return client.eof; });
    status(client.bytes, "201 Created"); common(client.bytes, job->context);
    const auto json = body(client.bytes);
    BOOST_CHECK_EQUAL(json["access_token"].get<std::string>(), token);
    BOOST_CHECK_EQUAL(json["token_type"].get<std::string>(), "Bearer");
    BOOST_CHECK_EQUAL(json["expires_at"].get<std::string>(), "1970-01-08T00:00:00.000000Z");
    BOOST_CHECK_EQUAL(json.size(), 4); BOOST_CHECK_EQUAL(f.codec.calls, 0);
}
BOOST_AUTO_TEST_CASE(logoutAndRevokeUseProtectedVoidCompletion) {
    Fixture f;
    auto wire = f.success<LogoutJob>(request("DELETE", logoutPath), Result<void>{});
    status(wire, "204 No Content"); common(wire, f.job<LogoutJob>()->context.requestId);
    BOOST_CHECK(wire.substr(wire.find("\r\n\r\n") + 4).empty());
    BOOST_CHECK(wire.find("content-type:") == std::string::npos);
    BOOST_CHECK(f.job<LogoutJob>()->context.credential.mode == CredentialMode::Bearer);
    Client client(f.server.listenAddress()); client.send(request("DELETE", revokePath));
    f.until([&] { return f.dispatcher.jobs.size() == 2; });
    auto job = f.job<RevokeJob>(1); BOOST_CHECK(job->command->id == iid);
    job->result.emplace(); f.dispatcher.release(job);
    f.until([&] { client.read(); return client.eof; });
    status(client.bytes, "204 No Content"); common(client.bytes, job->context.requestId);
    // 是否已经撤销由应用判断；成功仍为 204。
    status(f.success<RevokeJob>(request("DELETE", revokePath), Result<void>{}), "204 No Content");
}
BOOST_AUTO_TEST_CASE(createInviteDefaultsAndExplicitBounds) {
    Fixture f;
    for (const auto& input : {std::string("{}"), std::string(R"({"max_uses":10000,"valid_for_seconds":2592000})")}) {
        const auto index = f.dispatcher.jobs.size();
        Client client(f.server.listenAddress()); client.send(request("POST", invitesPath, auth, input));
        f.until([&] { return f.dispatcher.jobs.size() == index + 1; });
        auto job = f.job<CreateJob>(index);
        BOOST_CHECK_EQUAL(job->command->maxUses.value(), index == 0 ? 1 : 10000);
        BOOST_CHECK_EQUAL(job->command->validFor.value(), index == 0 ? 604800 : 2592000);
        job->result.emplace(created()); f.dispatcher.release(job);
        f.until([&] { client.read(); return client.eof; });
        status(client.bytes, "201 Created"); common(client.bytes, job->context.requestId);
        BOOST_CHECK_EQUAL(body(client.bytes)["token"].get<std::string>(), inviteToken);
        BOOST_CHECK_EQUAL(body(client.bytes)["invite"].size(), 7);
    }
}
BOOST_AUTO_TEST_CASE(inviteListRoundTripsBoundCursorAndIncludesRevokedMetadata) {
    Fixture f;
    auto info = invite(); info.revokedAt = UtcTime{};
    const InviteCursor cursor{uid, {info.createdAt, iid}};
    auto wire = f.success<ListJob>(request("GET", invitesPath), Page<InviteInfo, InviteCursor>{{info}, cursor});
    status(wire, "200 OK"); noSecret(wire);
    const auto json = body(wire); BOOST_CHECK_EQUAL(json["items"][0].size(), 7);
    BOOST_CHECK(json["items"][0]["revoked_at"].is_string());
    const auto encoded = json["next_cursor"].get<std::string>();
    Client client(f.server.listenAddress()); client.send(request("GET", invitesPath + "?limit=200&cursor=" + encoded));
    f.until([&] { return f.dispatcher.jobs.size() == 2; });
    auto job = f.job<ListJob>(1); BOOST_CHECK_EQUAL(job->command->size.value(), 200);
    BOOST_REQUIRE(job->command->after); BOOST_CHECK(job->command->after->userId == uid);
    BOOST_CHECK(job->command->after->position.id == iid);
    job->result.emplace(Page<InviteInfo, InviteCursor>{}); f.dispatcher.release(job);
    f.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(body(client.bytes)["items"].empty()); BOOST_CHECK(body(client.bytes)["next_cursor"].is_null());
}
BOOST_AUTO_TEST_CASE(anonymousRoutesRejectAnyCredentialHeaderBeforeContinue) {
    Fixture f;
    for (const auto& path : {registerPath, loginPath}) {
        for (const auto& headers : {auth, std::string("Authorization:\r\n"), std::string("Cookie:\r\n"), auth + auth}) {
            const auto wire = f.exchange(request("POST", path, headers + "Content-Type: application/json\r\nContent-Length: 100\r\nExpect: 100-continue\r\n"));
            status(wire, "400 Bad Request"); BOOST_CHECK(wire.find("100 Continue") == std::string::npos);
        }
    }
    BOOST_CHECK_EQUAL(f.codec.calls, 0); BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(protectedIdentityOperationsRequireCanonicalBearer) {
    Fixture f;
    for (const auto& route : {std::make_pair("DELETE", logoutPath), {"DELETE", revokePath}, {"GET", invitesPath}, {"POST", invitesPath}}) {
        const auto wire = f.exchange(request(route.first, route.second, ""));
        status(wire, "401 Unauthorized"); BOOST_CHECK(wire.find("www-authenticate: Bearer realm=\"anof\"") != std::string::npos);
        status(f.exchange(request(route.first, route.second, auth + "Cookie:\r\n")), "400 Bad Request");
        status(f.exchange(request(route.first, route.second, auth + auth)), "400 Bad Request");
    }
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(jsonRoutesRejectMalformedUnknownDuplicateAndOversizedInput) {
    Fixture f;
    for (const auto& path : {registerPath, loginPath, invitesPath}) {
        const auto headers = path == invitesPath ? auth : "";
        for (const auto& input : {std::string{}, std::string("[]"), std::string(R"({"unknown":1})"), std::string(R"({"max_uses":1,"max_uses":2})"), std::string(R"({"nested":{}})")})
            status(f.exchange(request("POST", path, headers, input)), "400 Bad Request");
        status(f.exchange(request("POST", path + "?unknown=1", headers, "{}")), "400 Bad Request");
        status(f.exchange(request("POST", path, headers + "Content-Type: text/plain\r\nContent-Length: 0\r\n")), "415 Unsupported Media Type");
        const auto wire = f.exchange(request("POST", path, headers + "Content-Type: application/json\r\nContent-Length: 65537\r\nExpect: 100-continue\r\n"));
        BOOST_CHECK(wire.find("HTTP/1.1 413") == 0); BOOST_CHECK(wire.find("100 Continue") == std::string::npos);
    }
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(bodylessRoutesRejectFramingAndInvalidQueries) {
    Fixture f;
    for (const auto& route : {std::make_pair("DELETE", logoutPath), {"DELETE", revokePath}, {"GET", invitesPath}}) {
        // 非零 Content-Length 先触发框架的零正文上限，其余由协议入口拒绝。
        status(f.exchange(request(route.first, route.second, auth + "Content-Length: 1\r\n")), "413 Content Too Large");
        for (const auto* framing : {"Transfer-Encoding: chunked\r\n", "Trailer: X-Test\r\n"})
            status(f.exchange(request(route.first, route.second, auth + framing)), "400 Bad Request");
        status(f.exchange(request(route.first, route.second + "?unknown=1")), "400 Bad Request");
    }
    for (const auto* query : {"?limit=0", "?limit=201", "?limit=1&limit=2", "?cursor=bad", "?cursor=&cursor="})
        status(f.exchange(request("GET", invitesPath + query)), "400 Bad Request");
    status(f.exchange(request("DELETE", invitesPath + "/bad")), "400 Bad Request");
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(submissionRateCapacityAndStoppingHaveDistinctErrors) {
    Fixture f;
    for (const auto& item : {std::make_pair(SubmitError::RateLimited, "429 Too Many Requests"), {SubmitError::QueueFull, "503 Service Unavailable"}, {SubmitError::Stopping, "503 Service Unavailable"}, {SubmitError::InternalError, "500 Internal Server Error"}}) {
        f.dispatcher.rejection = item.first;
        for (const auto& path : {registerPath, loginPath, invitesPath}) {
            auto wire = f.exchange(request("POST", path, path == invitesPath ? auth : "", path == registerPath ? registration : path == loginPath ? login : "{}"));
            status(wire, item.second); noSecret(wire);
            BOOST_CHECK_EQUAL(body(wire)["error"]["code"].get<std::string>(), item.first == SubmitError::RateLimited ? "RATE_LIMITED" : item.first == SubmitError::InternalError ? "INTERNAL_ERROR" : "TEMPORARILY_UNAVAILABLE");
        }
    }
    BOOST_CHECK(f.dispatcher.jobs.empty()); BOOST_CHECK_EQUAL(f.dispatcher.deliveries, 0);
}
BOOST_AUTO_TEST_CASE(applicationErrorsRemainSecretFreeAndSessionLimitIs409) {
    Fixture f;
    const auto wire = f.success<LoginJob>(request("POST", loginPath, "", login), ErrorCode::SessionLimitReached);
    status(wire, "409 Conflict"); BOOST_CHECK_EQUAL(body(wire)["error"]["code"].get<std::string>(), "SESSION_LIMIT_REACHED"); noSecret(wire);
    for (const auto error : {ErrorCode::CommitOutcomeUnknown, ErrorCode::InternalError}) {
        const auto loginWire = f.success<LoginJob>(request("POST", loginPath, "", login), error);
        const auto inviteWire = f.success<CreateJob>(request("POST", invitesPath, auth, "{}"), error);
        for (const auto& result : {loginWire, inviteWire}) { status(result, "500 Internal Server Error"); noSecret(result); BOOST_CHECK(body(result)["error"]["details"].empty()); }
    }
    status(f.success<RegisterJob>(request("POST", registerPath, "", registration), ErrorCode::InviteUnavailable), "409 Conflict");
    status(f.success<CreateJob>(request("POST", invitesPath, auth, "{}"), ErrorCode::Forbidden), "403 Forbidden");
    status(f.success<RevokeJob>(request("DELETE", revokePath), ErrorCode::NotFound), "404 Not Found");
    const auto expired = f.success<LogoutJob>(request("DELETE", logoutPath), ErrorCode::Unauthenticated);
    status(expired, "401 Unauthorized"); BOOST_CHECK(expired.find("www-authenticate:") != std::string::npos);
}
BOOST_AUTO_TEST_CASE(inviteCursorBindingIsCheckedByApplication) {
    Fixture f;
    const InviteCursor cursor{uid, {UtcTime{}, iid}};
    const auto wire = f.success<ListJob>(request("GET", invitesPath + "?cursor=" + encodeCursor(cursor).value()), ErrorCode::InvalidArgument);
    status(wire, "400 Bad Request"); BOOST_CHECK_EQUAL(f.dispatcher.submissions, 1);
}
BOOST_AUTO_TEST_CASE(methodPolicyHasNoAutomaticHeadOptionsOrCors) {
    Fixture f;
    for (const auto& route : {std::make_pair(registerPath, "POST"), {loginPath, "POST"}, {logoutPath, "DELETE"}, {revokePath, "DELETE"}, {invitesPath, "GET, POST"}}) {
        for (const auto* method : {"HEAD", "OPTIONS", "PUT"}) {
            auto wire = f.exchange(request(method, route.first));
            status(wire, "405 Method Not Allowed");
            BOOST_CHECK(wire.find(std::string("allow: ") + route.second + "\r\n") != std::string::npos);
            BOOST_CHECK(wire.find("access-control-") == std::string::npos);
        }
    }
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(timeoutCancelsPasswordPhaseButCannotCancelFinalTransaction) {
    Fixture f({128, 0.1, 0.03});
    for (const bool final : {false, true}) {
        const auto index = f.dispatcher.jobs.size();
        Client client(f.server.listenAddress()); client.send(request("POST", loginPath, "", login));
        f.until([&] { return f.dispatcher.jobs.size() == index + 1; });
        auto job = f.job<LoginJob>(index); BOOST_REQUIRE(f.dispatcher.startPassword(job));
        if (final) BOOST_REQUIRE(f.dispatcher.startFinal(job));
        f.until([&] { client.read(); return client.eof; });
        BOOST_CHECK_EQUAL(job->cancelled, !final);
        const auto before = client.bytes;
        job->result.emplace(loggedIn()); f.dispatcher.release(job);
        f.until([&] { return f.dispatcher.deliveries == index + 1; });
        client.read(); BOOST_CHECK_EQUAL(client.bytes, before); noSecret(client.bytes);
    }
    BOOST_CHECK_EQUAL(f.dispatcher.cancellations, 2);
}
BOOST_AUTO_TEST_CASE(fragmentedJsonWaitsForCompleteInputAndRejectsTrailers) {
    Fixture f;
    for (const auto& path : {registerPath, loginPath, invitesPath}) {
        const auto data = path == registerPath ? registration : path == loginPath ? login : "{}";
        const auto headers = path == invitesPath ? auth : "";
        const auto index = f.dispatcher.jobs.size();
        Client client(f.server.listenAddress());
        client.send("POST " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + headers +
            "Content-Type: application/json\r\nContent-Length: " + std::to_string(data.size()) + "\r\nExpect: 100-continue\r\n\r\n");
        f.until([&] { client.read(); return client.bytes.find("100 Continue") != std::string::npos; });
        BOOST_CHECK_EQUAL(f.dispatcher.jobs.size(), index);
        client.send(data);
        f.until([&] { return f.dispatcher.jobs.size() == index + 1; });
        if (path == registerPath) f.job<RegisterJob>(index)->result.emplace(user());
        else if (path == loginPath) f.job<LoginJob>(index)->result.emplace(loggedIn());
        else f.job<CreateJob>(index)->result.emplace(created());
        f.dispatcher.release(f.dispatcher.jobs[index]);
        f.until([&] { client.read(); return client.eof; });
        BOOST_CHECK(client.bytes.find("HTTP/1.1 201 Created\r\n") != std::string::npos);
        const auto before = f.dispatcher.submissions;
        const auto wire = f.exchange("POST " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + headers +
            "Content-Type: application/json\r\nTransfer-Encoding: chunked\r\nTrailer: X-Test\r\n\r\n2\r\n{}\r\n0\r\nX-Test: value\r\n\r\n");
        status(wire, "400 Bad Request"); BOOST_CHECK_EQUAL(f.dispatcher.submissions, before);
    }
}
BOOST_AUTO_TEST_CASE(bodyTimeoutAndLateInviteCreationDoNotReleaseSecrets) {
    Fixture f({128, 0.03, 0.03});
    const auto incomplete = f.exchange(request("POST", registerPath, "Content-Type: application/json\r\nContent-Length: 100\r\n") + "{");
    status(incomplete, "408 Request Timeout"); BOOST_CHECK(f.dispatcher.jobs.empty());
    Client client(f.server.listenAddress()); client.send(request("POST", invitesPath, auth, "{}"));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    auto job = f.job<CreateJob>(); BOOST_REQUIRE(f.dispatcher.startFinal(job));
    f.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(!job->cancelled); const auto before = client.bytes;
    job->result.emplace(created()); f.dispatcher.release(job);
    f.until([&] { return f.dispatcher.deliveries == 1; });
    client.read(); BOOST_CHECK_EQUAL(client.bytes, before); noSecret(client.bytes);
    // 后续交换使用自己的结果，不可能收到前一次的邀请明文。
    auto next = f.success<ListJob>(request("GET", invitesPath), Page<InviteInfo, InviteCursor>{});
    status(next, "200 OK"); noSecret(next);
}
BOOST_AUTO_TEST_CASE(readinessClosingDuringBodyPreventsAnonymousSubmission) {
    Fixture f;
    Client client(f.server.listenAddress());
    client.send("POST " + loginPath + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(login.size()) + "\r\nExpect: 100-continue\r\n\r\n");
    f.until([&] { client.read(); return client.bytes.find("100 Continue") != std::string::npos; });
    f.ready = false; client.send(login);
    f.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(client.bytes.find("HTTP/1.1 503 Service Unavailable") != std::string::npos);
    BOOST_CHECK(f.dispatcher.jobs.empty()); BOOST_CHECK_EQUAL(f.dispatcher.submissions, 0);
    noSecret(client.bytes);
}
BOOST_AUTO_TEST_CASE(httpCapacityReturnsWhileCancelledPasswordTaskStillOccupiesWorkerSlot) {
    Fixture f({1, 0.1, 0.03}); f.dispatcher.capacity = 1;
    Client first(f.server.listenAddress()); first.send(request("POST", loginPath, "", login));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    auto job = f.job<LoginJob>(); BOOST_REQUIRE(f.dispatcher.startPassword(job));
    status(f.exchange(request("GET", "/health/live", "")), "503 Service Unavailable");
    f.until([&] { first.read(); return first.eof; });
    BOOST_CHECK(job->cancelled); BOOST_CHECK_EQUAL(f.dispatcher.deliveries, 0);
    status(f.exchange(request("GET", "/health/live", "")), "200 OK");
    status(f.exchange(request("POST", loginPath, "", login)), "503 Service Unavailable");
    BOOST_CHECK_EQUAL(f.dispatcher.jobs.size(), 1);
    f.dispatcher.release(job); f.until([&] { return f.dispatcher.deliveries == 1; });
    status(f.success<LoginJob>(request("POST", loginPath, "", login), loggedIn()), "201 Created");
}
BOOST_AUTO_TEST_CASE(preAdmissionSubmitExceptionReleasesSlotAndNextRequestSucceeds) {
    Fixture f({1, 0.1, 0.1}); f.dispatcher.throwOnSubmit = true;
    const auto failed = f.exchange(request("POST", loginPath, "", login));
    BOOST_CHECK(failed.find("201 Created") == std::string::npos); noSecret(failed);
    BOOST_CHECK(f.dispatcher.jobs.empty());
    f.dispatcher.throwOnSubmit = false;
    status(f.success<LoginJob>(request("POST", loginPath, "", login), loggedIn()), "201 Created");
    BOOST_CHECK_EQUAL(f.dispatcher.deliveries, 1);
}
BOOST_AUTO_TEST_CASE(invalidSecretBearingResultsReturnInternalErrorWithoutToken) {
    Fixture f;
    auto result = loggedIn(); result.user.displayName = DisplayName::parse("valid").value();
    // 秘密类别不匹配是 dispatcher 输出错误；错误响应不得带出另一类 token。
    result.accessToken = SecretToken::invite(inviteToken).value();
    auto wire = f.success<LoginJob>(request("POST", loginPath, "", login), std::move(result));
    status(wire, "500 Internal Server Error"); noSecret(wire);
    auto invitation = created(); invitation.token = SecretToken::session(token).value();
    wire = f.success<CreateJob>(request("POST", invitesPath, auth, "{}"), std::move(invitation));
    status(wire, "500 Internal Server Error"); noSecret(wire);
}
