#define BOOST_TEST_MODULE AccountReads
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
using UsersJob = Fake::TypedJob<ListUsersCommand, Page<UserProfile, UserCursor>>;
using UserJob = Fake::TypedJob<GetUserCommand, AdminUserView>;
using QuotaJob = Fake::TypedJob<GetQuotaCommand, ManagedQuota>;
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
    HttpApi api;
    HttpServer server{&loop, InetAddress(0, true), "account-read-test"};
    bool stopped = false;
    explicit Fixture(HttpApiOptions options = {})
        : api(codec, identity, [this] { return ready; }, [](auto, const auto&, const auto&) -> HttpStreamCallbacks {
            BOOST_ERROR("Read routed to mkdir"); return {};
        }, dispatcher, {{}, [this](auto record) { finished.push_back(std::move(record)); }}, options) {
        api.configure(server); server.start();
    }
    void until(const std::function<bool()>& done) {
        if (done()) return;
        bool expired = false;
        const auto poll = loop.runEvery(0.001, [&] { if (done()) loop.quit(); });
        const auto timeout = loop.runAfter(3, [&] { expired = true; loop.quit(); });
        loop.loop(); loop.cancel(poll); loop.cancel(timeout);
        BOOST_REQUIRE(!expired);
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

BOOST_AUTO_TEST_CASE(userDetailDefersAndCarriesOwnedIdentityAndId) {
    Fixture f;
    Client client(f.server.listenAddress()); client.send(request(userPath));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    auto job = f.job<UserJob>();
    BOOST_CHECK(job->command->id == uid);
    BOOST_CHECK(job->context.credential.mode == CredentialMode::Bearer);
    BOOST_CHECK(job->context.credential.digest == SessionDigest(SessionDigest::Bytes{}));
    client.read(); BOOST_CHECK(client.bytes.empty());
    job->result.emplace(AdminUserView{user(), sid}); f.dispatcher.release(job);
    f.until([&] { client.read(); return client.eof; });
    status(client.bytes, "200 OK"); common(client.bytes, job->context.requestId);
    const auto json = body(client.bytes);
    BOOST_CHECK_EQUAL(json.size(), 2); BOOST_CHECK_EQUAL(json["user"].size(), 8);
    BOOST_CHECK(!json["user"]["enabled"].get<bool>());
    BOOST_CHECK_EQUAL(json["private_space_id"].get<std::string>(), sid.toString());
    BOOST_CHECK(client.bytes.find("etag: \"user-" + uid.toString() + "-9\"\r\n") != std::string::npos);
    BOOST_REQUIRE_EQUAL(f.finished.size(), 1);
    BOOST_CHECK(f.finished.front().requestId == job->context.requestId);
}
BOOST_AUTO_TEST_CASE(userListRoundTripsApplicationBoundCursor) {
    Fixture f;
    const auto caller = UserId::parse("40000000-0000-4000-8000-000000000002").value();
    Client client(f.server.listenAddress()); client.send(request(usersPath));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    auto first = f.job<UsersJob>(); BOOST_CHECK_EQUAL(first->command->size.value(), 100); BOOST_CHECK(!first->command->after);
    first->result.emplace(Page<UserProfile, UserCursor>{{user()}, UserCursor{caller, {uid}}}); f.dispatcher.release(first);
    f.until([&] { client.read(); return client.eof; });
    status(client.bytes, "200 OK"); common(client.bytes, first->context.requestId);
    BOOST_CHECK(client.bytes.find("etag:") == std::string::npos);
    const auto cursor = body(client.bytes)["next_cursor"].get<std::string>();
    BOOST_CHECK(std::get<UserCursor>(decodeUserCursor(cursor)).userId == caller);
    Client next(f.server.listenAddress()); next.send(request(usersPath + "?limit=200&cursor=" + cursor));
    f.until([&] { return f.dispatcher.jobs.size() == 2; });
    auto second = f.job<UsersJob>(1); BOOST_CHECK_EQUAL(second->command->size.value(), 200);
    BOOST_REQUIRE(second->command->after); BOOST_CHECK(second->command->after->userId == caller);
    BOOST_CHECK(second->command->after->position.id == uid);
    second->result.emplace(Page<UserProfile, UserCursor>{}); f.dispatcher.release(second);
    f.until([&] { next.read(); return next.eof; });
    BOOST_CHECK(body(next.bytes)["items"].empty()); BOOST_CHECK(body(next.bytes)["next_cursor"].is_null());
}
BOOST_AUTO_TEST_CASE(quotaRoutesCarryDistinctScopesAndReturnedEtags) {
    Fixture f;
    for (const auto& path : {quotaPath, contributionPath}) {
        const auto index = f.dispatcher.jobs.size();
        Client client(f.server.listenAddress()); client.send(request(path));
        f.until([&] { return f.dispatcher.jobs.size() == index + 1; });
        auto job = f.job<QuotaJob>(index);
        const QuotaScope expected = path == quotaPath ? QuotaScope(SpaceQuota{sid}) : QuotaScope(PublicContributionQuota{sid, uid});
        BOOST_CHECK(job->command->scope == expected);
        job->result.emplace(quota(expected)); f.dispatcher.release(job);
        f.until([&] { client.read(); return client.eof; });
        status(client.bytes, "200 OK"); common(client.bytes, job->context.requestId);
        const auto json = body(client.bytes);
        BOOST_CHECK_EQUAL(json.size(), 3); BOOST_CHECK_EQUAL(json["space_id"].get<std::string>(), sid.toString());
        BOOST_CHECK_EQUAL(json["quota"]["limit"]["bytes"].get<std::string>(), "1099511627776");
        const auto tag = path == quotaPath ? "quota-space-" + sid.toString() : "quota-contribution-" + sid.toString() + "-" + uid.toString();
        BOOST_CHECK(client.bytes.find("etag: \"" + tag + "-3\"\r\n") != std::string::npos);
        if (path == quotaPath) BOOST_CHECK(json["user_id"].is_null());
        else BOOST_CHECK_EQUAL(json["user_id"].get<std::string>(), uid.toString());
    }
}
BOOST_AUTO_TEST_CASE(allAccountReadsRequireBearerAndRejectCookies) {
    Fixture f;
    for (const auto& path : paths) {
        for (const auto& headers : {std::string{}, std::string("Authorization: Bearer invalid\r\n")}) {
            const auto wire = f.exchange(request(path, headers)); status(wire, "401 Unauthorized");
            BOOST_CHECK(wire.find("www-authenticate: Bearer realm=\"anof\"") != std::string::npos);
        }
        status(f.exchange(request(path, auth + auth)), "400 Bad Request");
        status(f.exchange(request(path, auth + "Cookie:\r\n")), "400 Bad Request");
    }
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(bodyFramingAndUnknownQueriesNeverSubmit) {
    Fixture f;
    for (const auto& path : paths) {
        const auto wire = f.exchange(request(path, auth + "Content-Length: 1\r\nExpect: 100-continue\r\n"));
        BOOST_CHECK(wire.find("HTTP/1.1 4") == 0); BOOST_CHECK(wire.find("100 Continue") == std::string::npos);
        for (const auto* framing : {"Transfer-Encoding: chunked\r\n", "Trailer: X-Test\r\n"})
            status(f.exchange(request(path, auth + framing)), "400 Bad Request");
        status(f.exchange(request(path + "?unknown=1")), "400 Bad Request");
        if (path != usersPath) status(f.exchange(request(path + "?limit=1")), "400 Bad Request");
    }
    for (const auto* query : {"?limit=0", "?limit=201", "?limit=01", "?limit=1&limit=2", "?cursor=bad", "?cursor=&cursor="})
        status(f.exchange(request(usersPath + query)), "400 Bad Request");
    for (const auto& path : {usersPath + "/bad", std::string("/api/v1/admin/spaces/bad/quota"), "/api/v1/admin/spaces/" + sid.toString() + "/contributions/bad/quota"})
        status(f.exchange(request(path)), "400 Bad Request");
    BOOST_CHECK(f.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(methodsAreExplicitAndConditionalReadsStillReturnFullRepresentation) {
    Fixture f;
    for (const auto& path : paths) {
        for (const auto* method : {"HEAD", "OPTIONS", "POST"}) {
            const auto wire = f.exchange(request(path, auth, method)); status(wire, "405 Method Not Allowed");
            const auto allow = path == usersPath ? "GET" : path == userPath ? "GET, PATCH" : "GET, PUT";
            BOOST_CHECK(wire.find(std::string("allow: ") + allow + "\r\n") != std::string::npos);
            BOOST_CHECK(wire.find("access-control-") == std::string::npos);
        }
    }
    const auto headers = auth + "If-None-Match: *\r\nIf-Match: malformed\r\nContent-Length: 0\r\n";
    status(f.success<UserJob>(request(userPath, headers), AdminUserView{user(), sid}), "200 OK");
    status(f.success<QuotaJob>(request(quotaPath, headers), quota(SpaceQuota{sid})), "200 OK");
    status(f.success<QuotaJob>(request(contributionPath, headers), quota(PublicContributionQuota{sid, uid})), "200 OK");
    status(f.success<UsersJob>(request(usersPath, headers), Page<UserProfile, UserCursor>{}), "200 OK");
}
BOOST_AUTO_TEST_CASE(rejectionsAndApplicationErrorsRemainSeparate) {
    Fixture f;
    for (const auto& entry : {std::pair{SubmitError::RateLimited, "429 Too Many Requests"}, {SubmitError::QueueFull, "503 Service Unavailable"}, {SubmitError::Stopping, "503 Service Unavailable"}, {SubmitError::InternalError, "500 Internal Server Error"}}) {
        f.dispatcher.rejection = entry.first;
        for (const auto& path : paths) status(f.exchange(request(path)), entry.second);
    }
    BOOST_CHECK(f.dispatcher.jobs.empty()); BOOST_CHECK_EQUAL(f.dispatcher.deliveries, 0);
    f.dispatcher.rejection.reset();
    for (const auto& entry : {std::pair{ErrorCode::Unauthenticated, "401 Unauthorized"}, {ErrorCode::Forbidden, "403 Forbidden"}, {ErrorCode::NotFound, "404 Not Found"}, {ErrorCode::IntegrityError, "500 Internal Server Error"}, {ErrorCode::RateLimited, "429 Too Many Requests"}}) {
        const std::vector<std::string> wires{
            f.success<UsersJob>(request(usersPath), entry.first), f.success<UserJob>(request(userPath), entry.first),
            f.success<QuotaJob>(request(quotaPath), entry.first), f.success<QuotaJob>(request(contributionPath), entry.first)};
        for (const auto& wire : wires) {
            status(wire, entry.second); BOOST_CHECK(wire.find("etag:") == std::string::npos);
            BOOST_CHECK(body(wire)["error"]["details"].empty()); BOOST_CHECK(wire.find("test-token") == std::string::npos);
        }
    }
}
BOOST_AUTO_TEST_CASE(validCursorBindingReachesApplicationBeforeAuthorizationErrors) {
    Fixture f;
    const auto cursor = encodeCursor(UserCursor{UserId::parse(sid.toString()).value(), {uid}}).value();
    for (const auto& entry : {std::pair{ErrorCode::Forbidden, "403 Forbidden"}, {ErrorCode::InvalidArgument, "400 Bad Request"}}) {
        const auto index = f.dispatcher.jobs.size();
        Client client(f.server.listenAddress()); client.send(request(usersPath + "?cursor=" + cursor));
        f.until([&] { return f.dispatcher.jobs.size() == index + 1; });
        auto job = f.job<UsersJob>(index); BOOST_REQUIRE(job->command->after);
        BOOST_CHECK_EQUAL(job->command->after->userId.toString(), sid.toString());
        job->result.emplace(entry.first); f.dispatcher.release(job);
        f.until([&] { client.read(); return client.eof; }); status(client.bytes, entry.second);
    }
}
BOOST_AUTO_TEST_CASE(accountReadsShareAdmissionAndFreshKeepAliveRequestIdentity) {
    Fixture f({1, 0.1, 0.1});
    Client client(f.server.listenAddress()); client.send(request(userPath, auth, "GET", false));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    status(f.exchange(request(quotaPath)), "503 Service Unavailable");
    auto first = f.job<UserJob>(); first->result.emplace(AdminUserView{user(), sid}); f.dispatcher.release(first);
    f.until([&] { client.read(); return client.bytes.find("private_space_id") != std::string::npos; });
    client.send(request(quotaPath)); f.until([&] { return f.dispatcher.jobs.size() == 2; });
    auto second = f.job<QuotaJob>(1); BOOST_CHECK(first->context.requestId != second->context.requestId);
    second->result.emplace(quota(SpaceQuota{sid})); f.dispatcher.release(second);
    f.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(client.bytes.find(second->context.requestId.toString()) != std::string::npos);
    f.ready = false; status(f.exchange(request(usersPath)), "503 Service Unavailable");
    BOOST_CHECK_EQUAL(f.dispatcher.jobs.size(), 2);
}
BOOST_AUTO_TEST_CASE(timeoutDiscardsLateAccountResultAndNextExchangeRemainsIndependent) {
    Fixture f({128, 0.1, 0.03});
    Client client(f.server.listenAddress()); client.send(request(userPath));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    auto job = f.job<UserJob>(); BOOST_REQUIRE(f.dispatcher.startFinal(job));
    f.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(!job->cancelled); const auto before = client.bytes;
    job->result.emplace(AdminUserView{user(), sid}); f.dispatcher.release(job);
    f.until([&] { return f.dispatcher.deliveries == 1; });
    client.read(); BOOST_CHECK_EQUAL(client.bytes, before);
    BOOST_CHECK(client.bytes.find("private_space_id") == std::string::npos);
    status(f.success<QuotaJob>(request(quotaPath), quota(SpaceQuota{sid})), "200 OK");
}
