#include <tests/support/EventLoopWait.h>
#define BOOST_TEST_MODULE AccountWrites
#include <boost/test/unit_test.hpp>
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/AccountResponse.h>
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
using RenameJob = Fake::TypedJob<UpdateDisplayNameCommand, UserProfile>;
using PasswordJob = Fake::TypedJob<ChangePasswordCommand, void>;
using UserJob = Fake::TypedJob<SetUserEnabledCommand, AdminUserView>;
using QuotaJob = Fake::TypedJob<SetQuotaLimitCommand, ManagedQuota>;
const auto uid = UserId::parse("40000000-0000-4000-8000-000000000001").value();
const auto sid = SpaceId::parse("10000000-0000-4000-8000-000000000001").value();
const std::string usersPath = "/api/v1/admin/users";
const std::string userPath = usersPath + "/" + uid.toString();
const std::string quotaPath = "/api/v1/admin/spaces/" + sid.toString() + "/quota";
const std::string contributionPath = "/api/v1/admin/spaces/" + sid.toString() + "/contributions/" + uid.toString() + "/quota";
const std::vector<std::string> paths{usersPath, userPath, quotaPath, contributionPath};
const std::string auth = "Authorization: Bearer test-token\r\n";
UserProfile user() {
    return {uid, LoginName::parse("alice").value(), DisplayName::parse("家庭").value(), UserRole::Member, false, AuthEpoch::from(1).value(), Revision::from(9).value(), UtcTime{}, UtcTime{}};
}
ManagedQuota quota(QuotaScope scope) {
    const QuotaDelta zero{ByteCount::from(0).value(), NodeCount::from(0).value()};
    const QuotaDelta limit{ByteCount::from(1099511627776LL).value(), NodeCount::from(100).value()};
    return {scope, {{limit, zero, zero}, Revision::from(3).value()}};
}
struct Route { std::string method, path, data, tag; };
const std::string userTag = "\"user-" + uid.toString() + "-9\"";
const std::vector<Route> routes{
    {"PATCH", "/api/v1/users/me", R"({"display_name":"新名字"})", userTag},
    {"POST", "/api/v1/users/me/password", R"({"current_password":"  password  ","new_password":"example-new-password"})", userTag},
    {"PATCH", userPath, R"({"enabled":true})", userTag},
    {"PUT", quotaPath, R"({"limit":{"bytes":"1099511627776","nodes":"100"}})", "\"quota-space-" + sid.toString() + "-3\""},
    {"PUT", contributionPath, R"({"limit":{"bytes":"1099511627776","nodes":"100"}})", "\"quota-contribution-" + sid.toString() + "-" + uid.toString() + "-3\""}};
std::string request(const Route& route, const std::string& headers = auth, bool condition = true) {
    return route.method + " " + route.path + " HTTP/1.1\r\nHost: localhost\r\nX-Request-ID: untrusted\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(route.data.size()) + "\r\n" + headers +
        (condition ? "If-Match: " + route.tag + "\r\n" : "") + "\r\n" + route.data;
}
void status(const std::string& wire, const char* value) { BOOST_CHECK(wire.find(std::string("HTTP/1.1 ") + value + "\r\n") == 0); }
Json body(const std::string& wire) { return Json::parse(wire.substr(wire.find("\r\n\r\n") + 4)); }
void common(const std::string& wire, RequestId id) {
    BOOST_CHECK(wire.find("x-request-id: " + id.toString() + "\r\n") != std::string::npos);
    BOOST_CHECK(wire.find("cache-control: no-store\r\n") != std::string::npos);

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
    HttpServer server{&loop, InetAddress(0, true), "account-write-test"};
    bool stopped = false;
    explicit Fixture(HttpApiOptions options = {})
        : api(codec, identity, [this] { return ready; }, [](auto, const auto&, const auto&) -> HttpStreamCallbacks {
            BOOST_ERROR("Account write routed to mkdir"); return {};
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

BOOST_AUTO_TEST_CASE(renameCapturesConditionAndUsesReturnedUserEtag) {
    Fixture f; Client client(f.server.listenAddress()); client.send(request(routes[0]));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    auto job = f.job<RenameJob>(); client.read(); BOOST_CHECK(client.bytes.empty());
    BOOST_CHECK(job->context.credential.mode == CredentialMode::Bearer);
    BOOST_CHECK(job->context.credential.digest == SessionDigest(SessionDigest::Bytes{}));
    BOOST_CHECK_EQUAL(job->command->displayName.value(), "新名字");
    BOOST_CHECK(std::get<UserCondition>(job->command->expected).id == uid);
    BOOST_CHECK_EQUAL(std::get<UserCondition>(job->command->expected).revision.value(), 9);
    auto result = user(); result.revision = Revision::from(10).value(); result.displayName = DisplayName::parse("新名字").value();
    job->result.emplace(result); f.dispatcher.release(job);
    f.until([&] { client.read(); return client.eof; });
    status(client.bytes, "200 OK"); common(client.bytes, job->context.requestId);
    BOOST_CHECK_EQUAL(body(client.bytes)["revision"].get<std::string>(), "10");
    BOOST_CHECK(client.bytes.find("etag: \"user-" + uid.toString() + "-10\"\r\n") != std::string::npos);
}
BOOST_AUTO_TEST_CASE(passwordCapturesTwoSecretsAndSocketSourceThenReturnsEmpty204) {
    Fixture f; Client client(f.server.listenAddress());
    client.send(request(routes[1], auth + "Forwarded: for=203.0.113.8\r\nX-Forwarded-For: 198.51.100.1\r\nX-Real-IP: ::1\r\n"));
    f.until([&] { return f.dispatcher.jobs.size() == 1; }); auto job = f.job<PasswordJob>();
    BOOST_CHECK_EQUAL(job->command->currentPassword.view(), "  password  ");
    BOOST_CHECK_EQUAL(job->command->newPassword.view(), "example-new-password");
    BOOST_CHECK(job->command->source == AuthenticationSource::ipv4({127, 0, 0, 1}));
    BOOST_CHECK(job->context.credential.mode == CredentialMode::Bearer);
    job->result.emplace(); f.dispatcher.release(job);
    f.until([&] { client.read(); return client.eof; });
    status(client.bytes, "204 No Content"); common(client.bytes, job->context.requestId);
    BOOST_CHECK(client.bytes.substr(client.bytes.find("\r\n\r\n") + 4).empty());
    for (const auto* forbidden : {"content-type:", "etag:", "access_token", "password", "set-cookie:"}) BOOST_CHECK(client.bytes.find(forbidden) == std::string::npos);
    BOOST_CHECK(!job->command); BOOST_CHECK_EQUAL(f.dispatcher.submissions, 1);
}
BOOST_AUTO_TEST_CASE(enabledAndQuotaCommandsPreserveTargetsAndReturnResourceTags) {
    Fixture f;
    for (std::size_t n = 2; n < routes.size(); ++n) {
        Client client(f.server.listenAddress()); client.send(request(routes[n]));
        const auto index = n - 2; f.until([&] { return f.dispatcher.jobs.size() == index + 1; });
        RequestId id = f.identity.forExchange(1, 1);
        if (n == 2) {
            auto job = f.job<UserJob>(index); BOOST_CHECK(job->command->id == uid); BOOST_CHECK(job->command->enabled);
            BOOST_CHECK(std::get<UserCondition>(job->command->expected).id == uid);
            auto value = user(); value.enabled = true; job->result.emplace(AdminUserView{value, sid}); id = job->context.requestId;
        } else {
            auto job = f.job<QuotaJob>(index);
            const QuotaScope scope = n == 3 ? QuotaScope(SpaceQuota{sid}) : QuotaScope(PublicContributionQuota{sid, uid});
            BOOST_CHECK(job->command->scope == scope);
            BOOST_CHECK(std::get<QuotaCondition>(job->command->expected).scope == scope);
            BOOST_CHECK_EQUAL(job->command->limit.bytes.value(), 1099511627776LL);
            BOOST_CHECK_EQUAL(job->command->limit.nodes.value(), 100);
            job->result.emplace(quota(scope)); id = job->context.requestId;
        }
        f.dispatcher.release(f.dispatcher.jobs[index]); f.until([&] { client.read(); return client.eof; });
        status(client.bytes, "200 OK"); common(client.bytes, id);
        BOOST_CHECK(client.bytes.find("etag: " + routes[n].tag + "\r\n") != std::string::npos);
    }
}
BOOST_AUTO_TEST_CASE(allWritesRequireOneStrongConditionBeforeContinue) {
    Fixture f;
    for (auto route : routes) {
        auto wire = f.exchange(request(route, auth, false)); status(wire, "428 Precondition Required");
        BOOST_CHECK_EQUAL(body(wire)["error"]["code"].get<std::string>(), "PRECONDITION_REQUIRED");
        for (const auto& tag : {std::string{}, std::string("*"), "W/" + route.tag, route.tag + ", " + route.tag, "\"user-" + uid.toString() + "-9223372036854775808\""}) {
            auto bad = route; bad.tag = tag; status(f.exchange(request(bad)), "400 Bad Request");
        }
        status(f.exchange(request(route, auth + "If-Match: " + route.tag + "\r\n")), "400 Bad Request");
        auto head = request(route, auth + "Expect: 100-continue\r\n", false); head.resize(head.find("\r\n\r\n") + 4);
        wire = f.exchange(head); status(wire, "428 Precondition Required"); BOOST_CHECK(wire.find("100 Continue") == std::string::npos);
    }
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(validWrongKindAndIdConditionsReachApplicationUnchanged) {
    Fixture f;
    for (std::size_t n = 0; n < routes.size(); ++n) {
        auto route = routes[n];
        route.tag = n < 3 ? routes[4].tag : "\"user-10000000-0000-4000-8000-000000000009-4\"";
        Client client(f.server.listenAddress()); client.send(request(route));
        f.until([&] { return f.dispatcher.jobs.size() == n + 1; });
        auto complete = [&](const auto& job) {
            if (n < 3) BOOST_CHECK(std::get<QuotaCondition>(job->command->expected).scope == QuotaScope(PublicContributionQuota{sid, uid}));
            else BOOST_CHECK_EQUAL(std::get<UserCondition>(job->command->expected).id.toString(), "10000000-0000-4000-8000-000000000009");
            job->result.emplace(ErrorCode::PreconditionFailed); f.dispatcher.release(job);
        };
        if (n == 0) complete(f.job<RenameJob>(n));
        else if (n == 1) complete(f.job<PasswordJob>(n));
        else if (n == 2) complete(f.job<UserJob>(n));
        else complete(f.job<QuotaJob>(n));
        f.until([&] { client.read(); return client.eof; }); status(client.bytes, "412 Precondition Failed");
        BOOST_CHECK(client.bytes.find("etag:") == std::string::npos); BOOST_CHECK(body(client.bytes)["error"]["details"].empty());
    }
}
BOOST_AUTO_TEST_CASE(credentialsAndMalformedBodyNeverSubmit) {
    Fixture f;
    for (auto route : routes) {
        status(f.exchange(request(route, "")), "401 Unauthorized");
        status(f.exchange(request(route, auth + auth)), "400 Bad Request");
        status(f.exchange(request(route, auth + "Cookie:\r\n")), "400 Bad Request");
        for (const auto* value : {"", "{}", "[]", R"({"role":"administrator"})", R"({"enabled":true,"enabled":false})", R"({"limit":{"bytes":"0","bytes":"1","nodes":"0"}})"}) {
            auto bad = route; bad.data = value; status(f.exchange(request(bad)), "400 Bad Request");
        }
        route.path += "?limit=1"; status(f.exchange(request(route)), "400 Bad Request");
    }
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(submissionAndCompletionFailuresHaveCorrectSecretFreeMappings) {
    Fixture f;
    for (const auto& item : {std::pair{SubmitError::RateLimited, "429 Too Many Requests"}, {SubmitError::QueueFull, "503 Service Unavailable"}, {SubmitError::Stopping, "503 Service Unavailable"}}) {
        f.dispatcher.rejection = item.first;
        for (const auto& route : routes) status(f.exchange(request(route)), item.second);
    }
    BOOST_CHECK(f.dispatcher.jobs.empty()); f.dispatcher.rejection.reset();
    for (const auto& item : {std::pair{ErrorCode::Unauthenticated, "401 Unauthorized"}, {ErrorCode::Forbidden, "403 Forbidden"}, {ErrorCode::NotFound, "404 Not Found"}, {ErrorCode::PreconditionFailed, "412 Precondition Failed"}, {ErrorCode::RateLimited, "429 Too Many Requests"}, {ErrorCode::IntegrityError, "500 Internal Server Error"}, {ErrorCode::CommitOutcomeUnknown, "500 Internal Server Error"}}) {
        std::vector<std::string> wires{f.success<RenameJob>(request(routes[0]), item.first), f.success<PasswordJob>(request(routes[1]), item.first), f.success<UserJob>(request(routes[2]), item.first), f.success<QuotaJob>(request(routes[3]), item.first), f.success<QuotaJob>(request(routes[4]), item.first)};
        for (const auto& wire : wires) {
            status(wire, item.second); BOOST_CHECK(wire.find("etag:") == std::string::npos);
            for (const auto* secret : {"example-new-password", "  password  ", "test-token", "access_token"}) BOOST_CHECK(wire.find(secret) == std::string::npos);
            BOOST_CHECK(body(wire)["error"]["details"].empty());
        }
    }
    for (std::size_t n = 3; n < 5; ++n) {
        auto wire = f.success<QuotaJob>(request(routes[n]), ErrorCode::QuotaExceeded);
        status(wire, "409 Conflict"); BOOST_CHECK_EQUAL(body(wire)["error"]["code"].get<std::string>(), "QUOTA_EXCEEDED");
    }
}
BOOST_AUTO_TEST_CASE(bodyWaitsForCompleteAndRejectsTrailersAndOversize) {
    Fixture f;
    auto input = request(routes[1], auth + "Expect: 100-continue\r\n"); const auto end = input.find("\r\n\r\n") + 4;
    Client client(f.server.listenAddress()); client.send(input.substr(0, end));
    f.until([&] { client.read(); return client.bytes.find("100 Continue") != std::string::npos; });
    BOOST_CHECK(f.dispatcher.jobs.empty()); client.send(input.substr(end));
    f.until([&] { return f.dispatcher.jobs.size() == 1; }); auto job = f.job<PasswordJob>(); job->result.emplace(); f.dispatcher.release(job);
    f.until([&] { client.read(); return client.eof; }); BOOST_CHECK(client.bytes.find("204 No Content") != std::string::npos);
    for (auto route : routes) {
        route.data.assign(65537, 'x'); auto head = request(route, auth + "Expect: 100-continue\r\n"); head.resize(head.find("\r\n\r\n") + 4);
        auto wire = f.exchange(head); BOOST_CHECK(wire.find("HTTP/1.1 413") == 0); BOOST_CHECK(wire.find("100 Continue") == std::string::npos);
        auto chunked = route.method + " " + route.path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + auth + "If-Match: " + route.tag + "\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nTrailer: X-Test\r\n\r\n2\r\n{}\r\n0\r\nX-Test: yes\r\n\r\n";
        status(f.exchange(chunked), "400 Bad Request");
    }
    BOOST_CHECK_EQUAL(f.dispatcher.jobs.size(), 1);
}
BOOST_AUTO_TEST_CASE(passwordTimeoutCancellationAndFinalCompletionDoNotWriteLate204) {
    Fixture f({128, 0.1, 0.03});
    for (const bool final : {false, true}) {
        const auto index = f.dispatcher.jobs.size(); Client client(f.server.listenAddress()); client.send(request(routes[1]));
        f.until([&] { return f.dispatcher.jobs.size() == index + 1; }); auto job = f.job<PasswordJob>(index);
        BOOST_REQUIRE(f.dispatcher.startPassword(job)); if (final) BOOST_REQUIRE(f.dispatcher.startFinal(job));
        f.until([&] { client.read(); return client.eof; }); const auto before = client.bytes;
        BOOST_CHECK_EQUAL(job->cancelled, !final); job->result.emplace(); f.dispatcher.release(job);
        f.until([&] { return f.dispatcher.deliveries == index + 1; }); client.read();
        BOOST_CHECK_EQUAL(client.bytes, before); BOOST_CHECK(client.bytes.find("204 No Content") == std::string::npos);
        BOOST_CHECK(!job->command);
    }
}
BOOST_AUTO_TEST_CASE(writeMethodsMediaAndPathsAreStrict) {
    Fixture f;
    for (std::size_t n = 0; n < routes.size(); ++n) {
        auto route = routes[n];
        const std::string allow = n == 1 ? "POST" : n < 3 ? "GET, PATCH" : "GET, PUT";
        for (const auto* method : {"HEAD", "OPTIONS", "DELETE"}) {
            auto wrong = route; wrong.method = method; const auto wire = f.exchange(request(wrong));
            status(wire, "405 Method Not Allowed");
            BOOST_CHECK(wire.find("allow: " + allow + "\r\n") != std::string::npos);
            BOOST_CHECK(wire.find("access-control-") == std::string::npos);
        }
        auto wire = request(route); const auto at = wire.find("application/json"); wire.replace(at, 16, "text/plain");
        status(f.exchange(wire), "415 Unsupported Media Type");
        status(f.exchange(request(route, auth + "Content-Encoding: gzip\r\n")), "415 Unsupported Media Type");
        if (n >= 2) {
            route.path = n == 2 ? "/api/v1/admin/users/bad" : n == 3 ? "/api/v1/admin/spaces/bad/quota" : "/api/v1/admin/spaces/" + sid.toString() + "/contributions/bad/quota";
            status(f.exchange(request(route)), "400 Bad Request");
        }
    }
    BOOST_CHECK(f.dispatcher.jobs.empty());
}

BOOST_AUTO_TEST_CASE(incompletePasswordInputNeverDispatchesAndReturnsCapacity) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f({1, 0.03, 1});
        Client client(f.server.listenAddress());
        const auto input = request(routes[1], auth + "Expect: 100-continue\r\n");
        const auto end = input.find("\r\n\r\n") + 4;
        client.send(input.substr(0, end));
        f.until([&] { client.read(); return client.bytes.find("100 Continue") != std::string::npos; });
        if (mode == 0) {
            client.send(input.substr(end, 20));
            ::shutdown(client.fd, SHUT_RDWR);
        } else if (mode == 1) {
            client.send(input.substr(end, 20));
        } else {
            f.ready = false;
            client.send(input.substr(end));
        }
        f.until([&] { client.read(); return !f.finished.empty() && (mode == 0 || client.eof); });
        BOOST_CHECK(f.dispatcher.jobs.empty());
        if (mode == 1) BOOST_CHECK(client.bytes.find("408 Request Timeout") != std::string::npos);
        if (mode == 2) BOOST_CHECK(client.bytes.find("503 Service Unavailable") != std::string::npos);
        BOOST_CHECK(client.bytes.find("example-new-password") == std::string::npos);
        f.ready = true;
        status(f.success<PasswordJob>(request(routes[1]), Result<void>{}), "204 No Content");
    }
}
BOOST_AUTO_TEST_CASE(forcedStopDoesNotWaitForPasswordTaskOrWriteLateResult) {
    for (int phase = 0; phase < 3; ++phase) {
        Fixture f;
        Client client(f.server.listenAddress()); client.send(request(routes[1]));
        f.until([&] { return f.dispatcher.jobs.size() == 1; });
        auto job = f.job<PasswordJob>();
        if (phase >= 1) BOOST_REQUIRE(f.dispatcher.startPassword(job));
        if (phase == 2) BOOST_REQUIRE(f.dispatcher.startFinal(job));
        std::size_t notifications = 0;
        BOOST_CHECK(f.server.stop(0, [&] { ++notifications; f.stopped = true; }));
        BOOST_CHECK(!f.server.stop(0, [] { BOOST_ERROR("Repeated stop replaced callback"); }));
        f.until([&] { client.read(); return f.stopped && client.eof; });
        BOOST_CHECK_EQUAL(f.dispatcher.deliveries, 0);
        BOOST_CHECK_EQUAL(f.dispatcher.cancellations, 1);
        BOOST_CHECK_EQUAL(job->cancelled, phase != 2);
        BOOST_CHECK(job->command.has_value());
        const auto before = client.bytes;
        job->result.emplace(ErrorCode::CommitOutcomeUnknown);
        f.dispatcher.release(job);
        f.until([&] { return f.dispatcher.deliveries == 1; }); client.read();
        BOOST_CHECK_EQUAL(client.bytes, before);
        BOOST_CHECK(client.bytes.find("204 No Content") == std::string::npos);
        BOOST_CHECK(client.bytes.find("password") == std::string::npos);
        BOOST_CHECK(!job->command);
        BOOST_CHECK_EQUAL(notifications, 1);
        BOOST_CHECK_EQUAL(f.finished.size(), 1);
    }
}
BOOST_AUTO_TEST_CASE(gracefulStopAllowsPassword204AndNotifiesOnce) {
    Fixture f;
    Client client(f.server.listenAddress()); client.send(request(routes[1]));
    f.until([&] { return f.dispatcher.jobs.size() == 1; }); auto job = f.job<PasswordJob>();
    BOOST_REQUIRE(f.dispatcher.startPassword(job)); BOOST_REQUIRE(f.dispatcher.startFinal(job));
    std::size_t notifications = 0;
    BOOST_CHECK(f.server.stop(0.5, [&] { ++notifications; f.stopped = true; }));
    BOOST_CHECK(!f.server.stop(0, [] { BOOST_ERROR("Repeated stop replaced callback"); }));
    BOOST_CHECK(!f.stopped);
    job->result.emplace(); f.dispatcher.release(job);
    f.until([&] { client.read(); return client.eof && f.stopped; });
    status(client.bytes, "204 No Content"); common(client.bytes, job->context.requestId);
    BOOST_CHECK(client.bytes.substr(client.bytes.find("\r\n\r\n") + 4).empty());
    BOOST_CHECK_EQUAL(notifications, 1);
    BOOST_CHECK_EQUAL(f.dispatcher.cancellations, 0);
    BOOST_CHECK(!job->command);
}
BOOST_AUTO_TEST_CASE(passwordTaskCapacityOutlivesHttpTimeout) {
    Fixture f({1, 1, 0.1}); f.dispatcher.capacity = 1;
    Client client(f.server.listenAddress()); client.send(request(routes[1]));
    f.until([&] { return f.dispatcher.jobs.size() == 1; }); auto job = f.job<PasswordJob>();
    BOOST_REQUIRE(f.dispatcher.startPassword(job));
    const std::string health = "GET /health/live HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    status(f.exchange(health), "503 Service Unavailable");
    f.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(job->cancelled); BOOST_CHECK_EQUAL(f.dispatcher.deliveries, 0);
    status(f.exchange(health), "200 OK");
    status(f.exchange(request(routes[1])), "503 Service Unavailable");
    BOOST_CHECK_EQUAL(f.dispatcher.jobs.size(), 1);
    f.dispatcher.release(job); f.until([&] { return f.dispatcher.deliveries == 1; });
    status(f.success<PasswordJob>(request(routes[1]), Result<void>{}), "204 No Content");
    BOOST_CHECK_EQUAL(f.dispatcher.deliveries, 2);
}
BOOST_AUTO_TEST_CASE(submitExceptionsReleaseAccountAdmissionWithoutLeakingSecrets) {
    Fixture f({1, 1, 1});
    for (const auto& route : routes) {
        f.dispatcher.throwOnSubmit = true;
        const auto wire = f.exchange(request(route));
        BOOST_CHECK(wire.find("200 OK") == std::string::npos);
        BOOST_CHECK(wire.find("204 No Content") == std::string::npos);
        for (const auto* secret : {"example-new-password", "  password  ", "test-token"})
            BOOST_CHECK(wire.find(secret) == std::string::npos);
        BOOST_CHECK_EQUAL(f.dispatcher.jobs.size(), f.dispatcher.deliveries);
        f.dispatcher.throwOnSubmit = false;
        status(f.success<PasswordJob>(request(routes[1]), Result<void>{}), "204 No Content");
    }
    BOOST_CHECK_EQUAL(f.dispatcher.submissions, 10);
    BOOST_CHECK_EQUAL(f.dispatcher.deliveries, 5);
}
BOOST_AUTO_TEST_CASE(password204ThenQuotaOnKeepAliveHaveIsolatedResponses) {
    Fixture f({1, 1, 1});
    Client client(f.server.listenAddress());
    auto first = request(routes[1]);
    first.erase(first.find("Connection: close\r\n"), std::string("Connection: close\r\n").size());
    client.send(first);
    f.until([&] { return f.dispatcher.jobs.size() == 1; }); auto password = f.job<PasswordJob>();
    password->result.emplace(); f.dispatcher.release(password);
    f.until([&] { client.read(); return client.bytes.find("\r\n\r\n") != std::string::npos; });
    status(client.bytes, "204 No Content"); common(client.bytes, password->context.requestId);
    const auto boundary = client.bytes.size();
    BOOST_CHECK_EQUAL(boundary, client.bytes.find("\r\n\r\n") + 4);
    BOOST_CHECK(client.bytes.find("etag:") == std::string::npos);
    BOOST_CHECK(client.bytes.find("content-type:") == std::string::npos);
    client.send(request(routes[4]));
    f.until([&] { return f.dispatcher.jobs.size() == 2; }); auto update = f.job<QuotaJob>(1);
    BOOST_CHECK(update->context.requestId != password->context.requestId);
    update->result.emplace(ErrorCode::QuotaExceeded); f.dispatcher.release(update);
    f.until([&] { client.read(); return client.eof; });
    const auto second = client.bytes.substr(boundary);
    status(second, "409 Conflict"); common(second, update->context.requestId);
    BOOST_CHECK_EQUAL(body(second)["error"]["code"].get<std::string>(), "QUOTA_EXCEEDED");
    BOOST_CHECK(second.find(password->context.requestId.toString()) == std::string::npos);
    BOOST_CHECK(client.bytes.find("password") == std::string::npos);
    BOOST_CHECK_EQUAL(f.dispatcher.deliveries, 2);
}
