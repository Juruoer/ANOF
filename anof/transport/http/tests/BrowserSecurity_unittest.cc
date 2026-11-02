#include <tests/support/EventLoopWait.h>
#define BOOST_TEST_MODULE BrowserSecurity
#include <anof/transport/http/BrowserSecurity.h>
#include <anof/transport/http/IdentityResponse.h>
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/RequestCredentials.h>
#include <muduox/net/EventLoop.h>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "HttpTestSupport.h"
#include "IdentityDispatcherFixture.h"
#include "UploadTestSupport.h"

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
const std::string registration = Json{ { "login", "Alice" }, { "display_name", "家庭" }, { "password", "  password  " }, { "invite_token", inviteToken } }.dump();
const std::string login = R"({"login":"Alice","password":"  password  ","credential_mode":"bearer"})";
const std::string registerPath = "/api/v1/auth/register";
const std::string loginPath = "/api/v1/auth/sessions";
const std::string logoutPath = "/api/v1/auth/session";
const std::string invitesPath = "/api/v1/admin/invites";
const std::string revokePath = invitesPath + "/" + iid.toString();
UserProfile user() {
    return { uid, LoginName::parse("alice").value(), DisplayName::parse("家庭").value(), UserRole::Member, true, AuthEpoch::from(1).value(), Revision::from(1).value(), UtcTime{}, UtcTime{} };
}

LoginResult loggedIn() { return { SecretToken::session(token).value(), UtcTime{} + std::chrono::hours(168), user() }; }

std::string request(const std::string& method, const std::string& path, const std::string& headers = auth, const std::optional<std::string>& data = std::nullopt) {
    return method + " " + path + " HTTP/1.1\r\nHost: drive.example\r\nX-Forwarded-For: 203.0.113.7\r\nX-Forwarded-Proto: https\r\nX-Request-ID: untrusted\r\nConnection: close\r\n" + headers +
           (data ? "Content-Type: application/json\r\nContent-Length: " + std::to_string(data->size()) + "\r\n" : "") + "\r\n" + data.value_or("");
}
void status(const std::string& wire, const char* value) { BOOST_CHECK_MESSAGE(wire.find(std::string("HTTP/1.1 ") + value + "\r\n") == 0, "Expected " << value << ", received " << wire.substr(0, wire.find("\r\n"))); }
Json body(const std::string& wire) { return Json::parse(wire.substr(wire.find("\r\n\r\n") + 4)); }
InstanceConfiguration formalConfiguration() {
    InstanceConfiguration c;
    c.developmentHttp = false;
    c.webEnabled = true;
    c.publicOrigin = "https://drive.example";
    c.trustedProxies = { "127.0.0.1" };
    return c;
}
class BrowserCodec final : public SessionTokenCodec {
public:
    mutable int calls = 0;
    Result<SessionDigest> digestBearer(std::string_view input) const override {
        ++calls;
        if (input != token && input != "test-token") return ErrorCode::Unauthenticated;
        return SessionDigest(SessionDigest::Bytes{});
    }
};
const std::string cookie = "Cookie: __Host-anof_session=" + token + "\r\n";
const std::string userCondition = "If-Match: \"user-" + uid.toString() + "-1\"\r\n";
const std::string same = "Origin: https://drive.example\r\nX-ANOF-Request: 1\r\n";
const std::string cookieLogin = R"({"login":"Alice","password":"  password  ","credential_mode":"cookie"})";
void cleared(const std::string& wire, bool expected = true) {
    BOOST_CHECK_EQUAL(wire.find("set-cookie:") != std::string::npos, expected);
    if (expected) BOOST_CHECK(wire.find(std::string("set-cookie: ") + std::string(kClearSessionCookie) + "\r\n") != std::string::npos);
}
class Fixture {
public:
    EventLoop loop;
    BrowserCodec codec;
    Identity identity;
    Fake dispatcher{ loop };
    bool ready = true;
    std::vector<HttpTraceRecord> finished;
    UploadFake uploadDispatcher{ loop };
    UnexpectedDriveDispatcher driveDispatcher;
    HttpApi api;
    HttpServer server{ &loop, InetAddress(0, true), "http-identity-test" };
    bool stopped = false;
    explicit Fixture(HttpApiOptions options = {})
        : api(codec, identity, [this] { return ready; }, [](auto, const auto&, const auto&) -> HttpStreamCallbacks {
            BOOST_ERROR("Identity routed to mkdir"); return {}; }, dispatcher, uploadDispatcher, driveDispatcher, formalConfiguration(), { {}, [this](auto record) { finished.push_back(std::move(record)); } }, options) {
        api.configure(server);
        server.start();
    }
    void until(const std::function<bool()>& done,
               const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        BOOST_REQUIRE(anof::test::waitFor(loop, done, 3, file, line));
    }
    ~Fixture() {
        server.stop(0, [this] { stopped = true; });
        dispatcher.releaseAll();
        for (auto& job : uploadDispatcher.jobs) uploadDispatcher.release(job);
        until([&] { return stopped && dispatcher.deliveries == dispatcher.jobs.size() && uploadDispatcher.deliveries == uploadDispatcher.jobs.size(); });
    }
    template <typename Job>
    std::shared_ptr<Job> job(std::size_t index = 0) {
        auto value = std::dynamic_pointer_cast<Job>(dispatcher.jobs.at(index));
        BOOST_REQUIRE(value);
        return value;
    }
    std::string exchange(const std::string& input) {
        Client client(server.listenAddress());
        client.send(input);
        until([&] { client.read(); return client.eof; });
        return client.bytes;
    }
    template <typename Job, typename Value>
    std::string success(const std::string& input, Value value) {
        const auto index = dispatcher.jobs.size();
        Client client(server.listenAddress());
        client.send(input);
        until([&] { return dispatcher.jobs.size() == index + 1; });
        auto task = job<Job>(index);
        if constexpr (std::is_same_v<Job, LoginJob> || std::is_same_v<Job, RegisterJob> || std::is_same_v<Job, Fake::TypedJob<ChangePasswordCommand, void>>)
            BOOST_CHECK(task->command->source == AuthenticationSource::ipv4({ 203, 0, 113, 7 }));
        task->result.emplace(std::move(value));
        dispatcher.release(task);
        until([&] { client.read(); return client.eof; });
        return client.bytes;
    }
};
}  // namespace

BOOST_AUTO_TEST_CASE(cookieParsingBoundsAmbiguityAndModes) {
    BrowserCodec codec;
    HttpHeaders h;
    h.append("Cookie", "theme=dark; preference=yes");
    h.append("Authorization", "Bearer test-token");
    auto credentials = readRequestCredentials(h, codec);
    BOOST_REQUIRE(std::holds_alternative<RequestCredentials>(credentials));
    BOOST_CHECK(std::get<RequestCredentials>(credentials).mode == CredentialMode::Bearer);
    h.clear();
    h.append("Cookie", "other=x; __Host-anof_session=" + token);
    credentials = readRequestCredentials(h, codec);
    BOOST_REQUIRE(std::holds_alternative<RequestCredentials>(credentials));
    BOOST_CHECK(std::get<RequestCredentials>(credentials).mode == CredentialMode::Cookie);
    for (const auto& value : { std::string{}, "\"" + token + "\"", "%41" + token.substr(1), std::string(43, 'B') }) {
        h.clear();
        h.append("Cookie", "__Host-anof_session=" + value);
        BOOST_CHECK(std::get<ProtocolError>(readRequestCredentials(h, codec)) == ProtocolError::Unauthenticated);
        h.append("Authorization", "invalid");
        BOOST_CHECK(std::get<ProtocolError>(readRequestCredentials(h, codec)) == ProtocolError::InvalidArgument);
    }
    for (const auto& value : { std::string("a=x; a=y"), "a=x; __Host-anof_session=" + token + "; a=z", std::string("bad name=x"), std::string("a=x\x01"), std::string("a=x;"), "a=" + std::string(8191, 'x') }) {
        h.clear();
        h.append("Cookie", value);
        BOOST_CHECK(std::holds_alternative<ProtocolError>(readCookieSession(h)));
    }
    std::string pairs;
    for (int i = 0; i < 64; ++i) pairs += (i ? "; " : "") + std::string("c") + std::to_string(i) + "=v";
    h.clear();
    h.append("Cookie", pairs);
    BOOST_CHECK(std::holds_alternative<std::optional<std::string_view>>(readCookieSession(h)));
    h.clear();
    h.append("Cookie", pairs + "; overflow=v");
    BOOST_CHECK(std::holds_alternative<ProtocolError>(readCookieSession(h)));
    h.clear();
    h.append("Cookie", "a=" + std::string(8190, 'x'));
    BOOST_CHECK(std::holds_alternative<std::optional<std::string_view>>(readCookieSession(h)));
    h.append("Cookie", "b=x");
    BOOST_CHECK(std::holds_alternative<ProtocolError>(readCookieSession(h)));
}
BOOST_AUTO_TEST_CASE(policyValidationAndOwnedConfiguration) {
    auto c = formalConfiguration();
    BrowserSecurity security(c);
    c.publicOrigin = "https://changed.example";
    c.trustedProxies.clear();
    HttpRequest r;
    const std::string method = "GET", path = "/api/v1/users/me";
    r.setMethod(method.data(), method.data() + method.size());
    r.setTarget(path.data(), path.data() + path.size());
    r.headers().append("Host", "drive.example");
    r.headers().append("X-Forwarded-For", "203.0.113.7");
    r.headers().append("X-Forwarded-Proto", "https");
    HttpRequestMetadata metadata;
    metadata.connection.peer = HttpEndpoint{ HttpEndpoint::Family::IPv4, "127.0.0.1", 100 };
    auto inspected = security.inspect(r, metadata);
    BOOST_REQUIRE(std::holds_alternative<BrowserRequest>(inspected));
    BOOST_CHECK(std::get<BrowserRequest>(inspected).source == AuthenticationSource::ipv4({ 203, 0, 113, 7 }));
    for (const auto& origin : { "http://drive.example", "https://Drive.example", "https://drive.example/", "https://drive.example:443", "https://u@drive.example" }) {
        c = formalConfiguration();
        c.publicOrigin = origin;
        BOOST_CHECK_THROW(BrowserSecurity{ c }, std::invalid_argument);
    }
    c = formalConfiguration();
    c.trustedProxies = { "10.0.0.1" };
    BOOST_CHECK_THROW(BrowserSecurity{ c }, std::invalid_argument);
    c = formalConfiguration();
    c.trustedProxies = { "::1", "::1" };
    BOOST_CHECK_THROW(BrowserSecurity{ c }, std::invalid_argument);
    c = formalConfiguration();
    c.listenAddress = "0.0.0.0";
    BOOST_CHECK_THROW(BrowserSecurity{ c }, std::invalid_argument);
    c = formalConfiguration();
    c.developmentHttp = true;
    BOOST_CHECK_THROW(BrowserSecurity{ c }, std::invalid_argument);
    BOOST_CHECK_NO_THROW(BrowserSecurity{ developmentConfiguration() });
    metadata.connection.peer->ip = "127.0.0.2";
    BOOST_CHECK(std::get<ProtocolError>(security.inspect(r, metadata)) == ProtocolError::InvalidArgument);
    r.headers().clear();
    r.headers().append("Host", "drive.example");
    BOOST_CHECK(std::get<ProtocolError>(security.inspect(r, metadata)) == ProtocolError::RequestOriginRejected);
}
BOOST_AUTO_TEST_CASE(proxyHostOriginAndCsrfRejectBeforeIdentity) {
    Fixture f;
    for (const auto& headers : { "Forwarded: for=203.0.113.2\r\n", "X-Real-IP: 203.0.113.2\r\n", "X-Forwarded-Port: 443\r\n", "X-Forwarded-For: 203.0.113.2\r\n", "Origin: https://drive.example\r\nOrigin: https://drive.example\r\n", "Origin: https://drive.example,https://drive.example\r\n", "X-ANOF-Request: 1\r\nX-ANOF-Request: 1\r\n" })
        status(f.exchange(request("GET", "/api/v1/users/me", auth + headers)), "400 Bad Request");
    for (const auto& origin : { "null", "http://drive.example", "https://evil.example", "https://drive.example/" }) {
        auto wire = f.exchange(request("DELETE", logoutPath, cookie + "Origin: " + origin + "\r\nX-ANOF-Request: 1\r\n"));
        status(wire, "403 Forbidden");
        cleared(wire, false);
    }
    for (const auto& headers : { cookie, cookie + "Origin: https://drive.example\r\n", auth + "Origin: https://drive.example\r\n", cookie + "Origin: https://drive.example\r\nX-ANOF-Request: 0\r\n" }) {
        auto wire = f.exchange(request("DELETE", logoutPath, headers));
        status(wire, "403 Forbidden");
        cleared(wire, false);
        BOOST_CHECK_EQUAL(body(wire)["error"]["code"].get<std::string>(), headers == cookie ? "REQUEST_ORIGIN_REJECTED" : "CSRF_REQUIRED");
    }
    auto wire = request("GET", "/api/v1/users/me", cookie);
    wire.replace(wire.find("Host: drive.example"), 19, "Host: evil.example");
    status(f.exchange(wire), "403 Forbidden");
    for (const auto& forwarded : { "203.0.113.7, 203.0.113.8", "203.0.113.7:42", "2001:0DB8::1", "unknown" }) {
        wire = request("GET", "/api/v1/users/me", cookie);
        wire.replace(wire.find("203.0.113.7"), 11, forwarded);
        status(f.exchange(wire), "400 Bad Request");
    }
    BOOST_CHECK(f.dispatcher.jobs.empty());
    BOOST_CHECK_EQUAL(f.codec.calls, 0);
}
BOOST_AUTO_TEST_CASE(cookieLoginAndRegistrationIgnoreOldIdentity) {
    Fixture f;
    const auto old = same + "Cookie: __Host-anof_session=" + token + "; theme=dark\r\n";
    auto malformed = f.exchange(request("POST", loginPath, same + "Cookie: __Host-anof_session=expired\r\n", cookieLogin));
    status(malformed, "401 Unauthorized");
    cleared(malformed, false);
    auto wire = f.success<LoginJob>(request("POST", loginPath, old, cookieLogin), loggedIn());
    status(wire, "201 Created");
    BOOST_CHECK(wire.find("set-cookie: __Host-anof_session=" + token + "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=604800\r\n") != std::string::npos);
    BOOST_CHECK(wire.find("Domain=") == std::string::npos);
    auto json = body(wire);
    BOOST_CHECK_EQUAL(json.size(), 3);
    BOOST_CHECK_EQUAL(json["credential_mode"], "cookie");
    BOOST_CHECK(json.dump().find(token) == std::string::npos);
    BOOST_CHECK(!json.contains("token_type"));
    BOOST_CHECK_EQUAL(f.codec.calls, 0);
    auto data = Json::parse(registration);
    data["credential_mode"] = "cookie";
    wire = f.success<RegisterJob>(request("POST", registerPath, old, data.dump()), user());
    status(wire, "201 Created");
    cleared(wire, false);
    BOOST_CHECK_EQUAL(body(wire).size(), 8);
    status(f.exchange(request("POST", loginPath, old, login)), "400 Bad Request");
    status(f.exchange(request("POST", registerPath, old, registration)), "400 Bad Request");
    status(f.exchange(request("POST", loginPath, same + auth, cookieLogin)), "400 Bad Request");
    status(f.exchange(request("POST", loginPath, "", cookieLogin)), "403 Forbidden");
    status(f.exchange(request("POST", loginPath, "Origin: https://drive.example\r\n", cookieLogin)), "403 Forbidden");
    wire = f.success<LoginJob>(request("POST", loginPath, "Cookie: theme=dark\r\n", login), loggedIn());
    status(wire, "201 Created");
    cleared(wire, false);
    BOOST_CHECK_EQUAL(body(wire)["access_token"], token);
}
BOOST_AUTO_TEST_CASE(meCarriesModeAndInstanceOnlyInMe) {
    Fixture f;
    using MeJob = Fake::TypedJob<GetMeCommand, CurrentUserView>;
    for (const auto& headers : { cookie, auth + "Cookie: theme=dark\r\n", cookie + "Origin: https://DRIVE.example:443\r\n" }) {
        const auto index = f.dispatcher.jobs.size();
        Client client(f.server.listenAddress());
        client.send(request("GET", "/api/v1/users/me", headers));
        f.until([&] { return f.dispatcher.jobs.size() == index + 1; });
        auto job = f.job<MeJob>(index);
        BOOST_CHECK(job->context.credential.mode == (headers == auth + "Cookie: theme=dark\r\n" ? CredentialMode::Bearer : CredentialMode::Cookie));
        job->result.emplace(CurrentUserView{ user(), instanceId });
        f.dispatcher.release(job);
        f.until([&] { client.read(); return client.eof; });
        status(client.bytes, "200 OK");
        cleared(client.bytes, false);
        BOOST_CHECK_EQUAL(body(client.bytes)["instance_id"], instanceId.toString());
        BOOST_CHECK_EQUAL(body(client.bytes).size(), 9);
        BOOST_CHECK(client.bytes.find("etag: \"user-" + uid.toString() + "-1\"\r\n") != std::string::npos);
    }
    auto wire = f.success<MeJob>(request("GET", "/api/v1/users/me", cookie), ErrorCode::Unauthenticated);
    status(wire, "401 Unauthorized");
}
BOOST_AUTO_TEST_CASE(cookieFailureAndUnknownResultsClearWithoutNewToken) {
    Fixture f;
    using PasswordJob = Fake::TypedJob<ChangePasswordCommand, void>;
    for (auto error : { ErrorCode::Unauthenticated, ErrorCode::CommitOutcomeUnknown, ErrorCode::Forbidden, ErrorCode::SessionLimitReached }) {
        auto wire = f.success<LoginJob>(request("POST", loginPath, same, cookieLogin), error);
        cleared(wire);
        BOOST_CHECK(wire.find(token) == std::string::npos);
        wire = f.success<LogoutJob>(request("DELETE", logoutPath, cookie + same), error);
        cleared(wire);
        wire = f.success<PasswordJob>(request("POST", "/api/v1/users/me/password", cookie + same + userCondition, R"({"current_password":"old-password","new_password":"new-password"})"), error);
        cleared(wire);
        wire = f.success<LogoutJob>(request("DELETE", logoutPath, auth), error);
        cleared(wire, false);
    }
    auto wire = f.exchange(request("DELETE", logoutPath, same));
    status(wire, "401 Unauthorized");
    cleared(wire);
    wire = f.exchange(request("DELETE", logoutPath, "Cookie: __Host-anof_session=invalid\r\n" + same));
    status(wire, "401 Unauthorized");
    cleared(wire, false);
    wire = f.success<LogoutJob>(request("DELETE", logoutPath, cookie + same), Result<void>{});
    status(wire, "204 No Content");
    cleared(wire);
    f.dispatcher.rejection = SubmitError::QueueFull;
    wire = f.exchange(request("POST", loginPath, same, cookieLogin));
    status(wire, "503 Service Unavailable");
    cleared(wire);
    wire = f.exchange(request("DELETE", logoutPath, cookie + same));
    status(wire, "503 Service Unavailable");
    cleared(wire);
    f.ready = false;
    wire = f.exchange(request("DELETE", logoutPath, cookie + same));
    status(wire, "503 Service Unavailable");
    cleared(wire);
    wire = f.exchange(request("POST", loginPath, same, cookieLogin));
    status(wire, "503 Service Unavailable");
    cleared(wire, false);
}
BOOST_AUTO_TEST_CASE(cookieTimeoutFrameworkEarlyResponseAndLateCompletion) {
    Fixture f({ 1, 0.03, 0.03 });
    Client client(f.server.listenAddress());
    client.send(request("POST", loginPath, same, cookieLogin));
    f.until([&] { return f.dispatcher.jobs.size() == 1; });
    auto job = f.job<LoginJob>();
    BOOST_REQUIRE(f.dispatcher.startPassword(job));
    BOOST_REQUIRE(f.dispatcher.startFinal(job));
    auto wire = f.exchange(request("DELETE", logoutPath, cookie + same));
    status(wire, "503 Service Unavailable");
    cleared(wire);
    f.until([&] { client.read(); return client.eof; });
    cleared(client.bytes);
    const auto before = client.bytes;
    job->result.emplace(loggedIn());
    f.dispatcher.release(job);
    f.until([&] { return f.dispatcher.deliveries == 1; });
    client.read();
    BOOST_CHECK_EQUAL(client.bytes, before);
    BOOST_CHECK(client.bytes.find(token) == std::string::npos);
    wire = f.exchange(request("DELETE", logoutPath, cookie + same + "Content-Length: 1\r\n"));
    status(wire, "413 Content Too Large");
    cleared(wire);
    wire = f.exchange(request("POST", "/api/v1/users/me/password", cookie + same + userCondition + "Content-Type: application/json\r\nContent-Length: 100\r\n") + "{");
    status(wire, "408 Request Timeout");
    cleared(wire);
    // 正文尚未解析，不能断言登录模式；不得误清 Bearer 登录的 Cookie。
    wire = f.exchange(request("POST", loginPath, same + "Content-Type: application/json\r\nContent-Length: 100\r\n") + "{");
    status(wire, "408 Request Timeout");
    cleared(wire, false);
}
BOOST_AUTO_TEST_CASE(uploadAndDownloadUseSameSecurityAndCredentialMode) {
    Fixture f;
    const std::string content = "/api/v1/nodes/" + uploadParent.toString() + "/content";
    for (const auto& method : { "GET", "HEAD" }) {
        const auto index = f.uploadDispatcher.jobs.size();
        Client client(f.server.listenAddress());
        client.send(request(method, content, cookie));
        f.until([&] { return f.uploadDispatcher.jobs.size() == index + 1; });
        auto job = std::dynamic_pointer_cast<UploadTypedJob<OpenDownloadCommand, OpenedDownload>>(f.uploadDispatcher.jobs[index]);
        BOOST_REQUIRE(job);
        BOOST_CHECK(job->context.credential.mode == CredentialMode::Cookie);
        job->result.emplace(ErrorCode::IntegrityError);
        f.uploadDispatcher.release(job);
        f.until([&] { client.read(); return client.eof; });
        status(client.bytes, "500 Internal Server Error");
        if (std::string(method) == "GET") BOOST_CHECK_EQUAL(body(client.bytes)["error"]["code"], "INTEGRITY_ERROR");
    }
    const auto headers = cookie + "Content-Type: application/offset+octet-stream\r\nUpload-Offset: 0\r\nContent-Length: 3\r\n";
    status(f.exchange(request("PATCH", uploadPath, headers) + "abc"), "403 Forbidden");
    Client client(f.server.listenAddress());
    client.send(request("PATCH", uploadPath, headers + same) + "abc");
    f.until([&] { return f.uploadDispatcher.jobs.size() == 3; });
    auto job = std::dynamic_pointer_cast<UploadTypedJob<PreparePatchCommand, PatchPreparation>>(f.uploadDispatcher.jobs.back());
    BOOST_REQUIRE(job);
    BOOST_CHECK(job->context.credential.mode == CredentialMode::Cookie);
    job->result.emplace(ErrorCode::Unauthenticated);
    f.uploadDispatcher.release(job);
    f.until([&] { client.read(); return client.eof; });
    status(client.bytes, "401 Unauthorized");
}
BOOST_AUTO_TEST_CASE(disabledCookiesAndCanonicalIpv6Sources) {
    HttpRequest r;
    std::string method = "GET", path = "/api/v1/users/me";
    r.setMethod(method.data(), method.data() + method.size());
    r.setTarget(path.data(), path.data() + path.size());
    r.headers().append("Host", "drive.example");
    r.headers().append("X-Forwarded-For", "2001:db8::1");
    r.headers().append("X-Forwarded-Proto", "https");
    HttpRequestMetadata m;
    m.connection.peer = HttpEndpoint{ HttpEndpoint::Family::IPv6, "::1", 100 };
    auto c = formalConfiguration();
    c.trustedProxies = { "::1" };
    auto inspected = BrowserSecurity(c).inspect(r, m);
    BOOST_REQUIRE(std::holds_alternative<BrowserRequest>(inspected));
    std::array<std::uint8_t, 16> bytes{};
    bytes[0] = 0x20;
    bytes[1] = 1;
    bytes[2] = 0x0d;
    bytes[3] = 0xb8;
    bytes[15] = 1;
    BOOST_CHECK(std::get<BrowserRequest>(inspected).source == AuthenticationSource::ipv6(bytes));
    r.headers().append("Cookie", "__Host-anof_session=" + token);
    c.webEnabled = false;
    BOOST_CHECK(std::get<ProtocolError>(BrowserSecurity(c).inspect(r, m)) == ProtocolError::CookieModeDisabled);
    r.headers().clear();
    r.headers().append("Cookie", "__Host-anof_session=" + token);
    BOOST_CHECK(std::get<ProtocolError>(BrowserSecurity(developmentConfiguration()).inspect(r, m)) == ProtocolError::CookieModeDisabled);
    r.headers().clear();
    r.headers().append("Cookie", "unrelated=x");
    BOOST_CHECK(std::holds_alternative<BrowserRequest>(BrowserSecurity(developmentConfiguration()).inspect(r, m)));
    const auto src = AuthenticationSource::ipv4({ 127, 0, 0, 1 });
    for (const auto& mode : { "cookie", "bearer" }) {
        auto data = Json::parse(registration);
        data["credential_mode"] = mode;
        auto parsed = parseRegisterJson(data.dump(), src);
        BOOST_REQUIRE(std::holds_alternative<RegistrationRequest>(parsed));
        BOOST_CHECK(std::get<RegistrationRequest>(parsed).mode == (std::string(mode) == "cookie" ? CredentialMode::Cookie : CredentialMode::Bearer));
    }
    for (const auto& mode : { Json("Cookie"), Json(""), Json(nullptr), Json(1) }) {
        auto data = Json::parse(registration);
        data["credential_mode"] = mode;
        BOOST_CHECK(std::holds_alternative<ProtocolError>(parseRegisterJson(data.dump(), src)));
    }
}

BOOST_AUTO_TEST_CASE(all41OperationsShareOriginProtectionBeforeDispatch) {
    Fixture f;
    const std::pair<const char*, const char*> routes[] = {
        { "POST", "/api/v1/nodes" },
        { "GET", "/health/live" },
        { "GET", "/health/ready" },
        { "POST", "/api/v1/auth/register" },
        { "POST", "/api/v1/auth/sessions" },
        { "DELETE", "/api/v1/auth/session" },
        { "PATCH", "/api/v1/users/me" },
        { "GET", "/api/v1/users/me" },
        { "POST", "/api/v1/admin/invites" },
        { "GET", "/api/v1/admin/invites" },
        { "DELETE", "/api/v1/admin/invites/40000000-0000-4000-8000-000000000001" },
        { "GET", "/api/v1/spaces" },
        { "GET", "/api/v1/nodes/40000000-0000-4000-8000-000000000001" },
        { "GET", "/api/v1/nodes/40000000-0000-4000-8000-000000000001/children" },
        { "POST", "/api/v1/users/me/password" },
        { "GET", "/api/v1/admin/users" },
        { "GET", "/api/v1/admin/users/40000000-0000-4000-8000-000000000001" },
        { "PATCH", "/api/v1/admin/users/40000000-0000-4000-8000-000000000001" },
        { "GET", "/api/v1/admin/spaces/40000000-0000-4000-8000-000000000001/quota" },
        { "PUT", "/api/v1/admin/spaces/40000000-0000-4000-8000-000000000001/quota" },
        { "GET", "/api/v1/admin/spaces/40000000-0000-4000-8000-000000000001/contributions/40000000-0000-4000-8000-000000000001/quota" },
        { "PUT", "/api/v1/admin/spaces/40000000-0000-4000-8000-000000000001/contributions/40000000-0000-4000-8000-000000000001/quota" },
        { "POST", "/api/v1/uploads" },
        { "GET", "/api/v1/uploads" },
        { "GET", "/api/v1/uploads/40000000-0000-4000-8000-000000000001" },
        { "HEAD", "/api/v1/uploads/40000000-0000-4000-8000-000000000001" },
        { "PATCH", "/api/v1/uploads/40000000-0000-4000-8000-000000000001" },
        { "DELETE", "/api/v1/uploads/40000000-0000-4000-8000-000000000001" },
        { "POST", "/api/v1/uploads/40000000-0000-4000-8000-000000000001/complete" },
        { "GET", "/api/v1/nodes/40000000-0000-4000-8000-000000000001/content" },
        { "HEAD", "/api/v1/nodes/40000000-0000-4000-8000-000000000001/content" },
        { "GET", "/api/v1/admin/storage" },
        { "PUT", "/api/v1/admin/storage" },
        { "PATCH", "/api/v1/nodes/40000000-0000-4000-8000-000000000001" },
        { "DELETE", "/api/v1/nodes/40000000-0000-4000-8000-000000000001" },
        { "POST", "/api/v1/nodes/40000000-0000-4000-8000-000000000001/copy" },
        { "GET", "/api/v1/search" },
        { "GET", "/api/v1/trash" },
        { "GET", "/api/v1/trash/40000000-0000-4000-8000-000000000001" },
        { "DELETE", "/api/v1/trash/40000000-0000-4000-8000-000000000001" },
        { "POST", "/api/v1/trash/40000000-0000-4000-8000-000000000001/restore" },
    };
    for (const auto& route : routes) {
        const auto wire = f.exchange(request(route.first, route.second, "Origin: https://evil.example\r\n"));
        status(wire, "403 Forbidden");
        cleared(wire, false);
        BOOST_CHECK(wire.find("access-control-") == std::string::npos);
    }
    BOOST_CHECK(f.dispatcher.jobs.empty());
    BOOST_CHECK(f.uploadDispatcher.jobs.empty());
    BOOST_CHECK_EQUAL(f.codec.calls, 0);
}
