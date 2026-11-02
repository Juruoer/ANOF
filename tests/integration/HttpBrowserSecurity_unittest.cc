#define BOOST_TEST_MODULE HttpBrowserSecurityIntegration
#include <tests/integration/HttpInstanceFixture.h>
namespace {
const std::string same = "Origin: https://cloud.test\r\nX-ANOF-Request: 1\r\n";
std::string header(const std::string& wire, const std::string& name) {
    const auto begin = wire.find("\r\n" + name + ": ");
    if (begin == std::string::npos) return {};
    const auto start = begin + name.size() + 4;
    return wire.substr(start, wire.find("\r\n", start) - start);
}
Json body(const std::string& wire) { return Json::parse(wire.substr(wire.find("\r\n\r\n") + 4)); }
void status(const std::string& wire, int expected) {
    // 不在失败消息打印Cookie/token/正文。
    BOOST_REQUIRE_MESSAGE(wire.rfind("HTTP/1.1 " + std::to_string(expected) + " ", 0) == 0,
                          "expected=" << expected << " received=" << wire.substr(0, wire.find("\r\n")));
}
std::string cookieHeader(const std::string& wire) {
    const auto value = header(wire, "set-cookie");
    BOOST_REQUIRE(value.rfind("__Host-anof_session=", 0) == 0);
    return "Cookie: " + value.substr(0, value.find(';')) + "\r\n";
}
void cleared(const std::string& wire) { BOOST_CHECK(header(wire, "set-cookie").find("Max-Age=0") != std::string::npos); }
struct BrowserFixture : Fixture {
    BrowserFixture() : Fixture(1048576, true) {}
    std::string send(std::string method, std::string path, const Json& data = nullptr, std::string headers = {}, std::string token = {}, std::string source = "203.0.113.1") {
        auto wire = request(method, path, data, token, "X-Forwarded-For: " + source + "\r\nX-Forwarded-Proto: https\r\n" + headers);
        wire.replace(wire.find("Host: localhost"), 15, "Host: cloud.test");
        return exchange(wire);
    }
    std::string cookieLogin(std::string name = "administrator", std::string source = "203.0.113.1") {
        auto data = loginBody(name);
        data["credential_mode"] = "cookie";
        auto wire = send("POST", "/api/v1/auth/sessions", data, same, {}, source);
        status(wire, 201);
        const auto value = body(wire);
        BOOST_CHECK(!value.contains("access_token") && !value.contains("token_type"));
        BOOST_CHECK_EQUAL(value.at("credential_mode"), "cookie");
        const auto set = header(wire, "set-cookie");
        for (auto flag : { "Secure", "HttpOnly", "SameSite=Strict", "Path=/", "Max-Age=604800" }) BOOST_CHECK(set.find(flag) != std::string::npos);
        BOOST_CHECK(set.find("Domain=") == std::string::npos);
        return cookieHeader(wire);
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(real_sessions_modes_origin_csrf_logout_and_restart) {
    BrowserFixture f;
    f.start();
    const auto cookie = f.cookieLogin();
    auto login = f.send("POST", "/api/v1/auth/sessions", loginBody());
    status(login, 201);
    BOOST_CHECK(header(login, "set-cookie").empty());
    const std::string token = body(login).at("access_token");
    auto me = f.send("GET", "/api/v1/users/me", nullptr, cookie);
    status(me, 200);
    BOOST_CHECK(InstanceId::parse(body(me).at("instance_id").get<std::string>()));
    auto otherMode = f.send("GET", "/api/v1/users/me", nullptr, "Cookie: __Host-anof_session=" + token + "\r\n");
    status(otherMode, 401);
    const auto cookieToken = cookie.substr(28, 43);
    status(f.send("GET", "/api/v1/users/me", nullptr, {}, cookieToken), 401);
    status(f.send("GET", "/api/v1/users/me", nullptr, cookie, token), 400);
    const auto path = "/api/v1/auth/session";
    auto badOrigin = f.send("DELETE", path, nullptr, cookie + "Origin: https://evil.test\r\nX-ANOF-Request: 1\r\n");
    status(badOrigin, 403);
    BOOST_CHECK(header(badOrigin, "set-cookie").empty());
    auto noCsrf = f.send("DELETE", path, nullptr, cookie + "Origin: https://cloud.test\r\n");
    status(noCsrf, 403);
    BOOST_CHECK_EQUAL(body(noCsrf)["error"]["code"], "CSRF_REQUIRED");
    BOOST_CHECK(header(noCsrf, "set-cookie").empty());
    f.stop();
    f.start();
    status(f.send("GET", "/api/v1/users/me", nullptr, cookie), 200);
    auto logout = f.send("DELETE", path, nullptr, cookie + same);
    status(logout, 204);
    cleared(logout);
    auto repeated = f.send("DELETE", path, nullptr, cookie + same);
    status(repeated, 401);
    cleared(repeated);
    status(f.send("GET", "/api/v1/users/me", nullptr, {}, token), 200);
    auto empty = f.send("DELETE", path, nullptr, same);
    status(empty, 401);
    cleared(empty);
    auto direct = f.exchange(request("GET", "/api/v1/users/me", nullptr, token));
    status(direct, 400);
}
BOOST_AUTO_TEST_CASE(cookie_registration_private_visibility_and_password_epoch) {
    BrowserFixture f;
    f.start();
    auto admin = f.cookieLogin();
    auto invite = f.send("POST", "/api/v1/admin/invites", { { "max_uses", 2 } }, admin + same);
    status(invite, 201);
    for (auto name : { "cookie_one", "cookie_two" }) {
        auto data = registration(name, body(invite)["token"]);
        data["credential_mode"] = "cookie";
        auto registered = f.send("POST", "/api/v1/auth/register", data, same);
        status(registered, 201);
        BOOST_CHECK(header(registered, "set-cookie").empty());
    }
    auto one = f.cookieLogin("cookie_one", "203.0.113.2");
    auto two = f.cookieLogin("cookie_two", "203.0.113.3");
    auto spaces = f.send("GET", "/api/v1/spaces", nullptr, one);
    status(spaces, 200);
    std::string privateRoot;
    const auto spaceData = body(spaces);
    for (const auto& item : spaceData["items"])
        if (item["kind"] == "private") privateRoot = item["root_node_id"];
    BOOST_REQUIRE(!privateRoot.empty());
    status(f.send("GET", "/api/v1/nodes/" + privateRoot, nullptr, two), 404);
    auto me = f.send("GET", "/api/v1/users/me", nullptr, one);
    status(me, 200);
    auto changed = f.send("POST", "/api/v1/users/me/password", { { "current_password", "fixture password only" }, { "new_password", "changed fixture password" } }, one + same + "If-Match: " + header(me, "etag") + "\r\n", {}, "203.0.113.2");
    status(changed, 204);
    cleared(changed);
    status(f.send("GET", "/api/v1/users/me", nullptr, one), 401);
    auto secondMe = f.send("GET", "/api/v1/users/me", nullptr, two);
    status(secondMe, 200);
    const auto userPath = "/api/v1/admin/users/" + body(secondMe).at("id").get<std::string>();
    auto disabled = f.send("PATCH", userPath, { { "enabled", false } }, admin + same + "If-Match: " + header(secondMe, "etag") + "\r\n");
    status(disabled, 200);
    status(f.send("GET", "/api/v1/users/me", nullptr, two), 401);
    auto enabled = f.send("PATCH", userPath, { { "enabled", true } }, admin + same + "If-Match: " + header(disabled, "etag") + "\r\n");
    status(enabled, 200);
    status(f.send("GET", "/api/v1/users/me", nullptr, two), 401);
}
BOOST_AUTO_TEST_CASE(cookie_unknown_commit_clears_without_exposing_secret_or_retrying) {
    BrowserFixture f;
    f.start({}, true);
    auto data = loginBody();
    data["credential_mode"] = "cookie";
    auto wire = f.send("POST", "/api/v1/auth/sessions", data, same);
    status(wire, 500);
    cleared(wire);
    BOOST_CHECK_EQUAL(body(wire)["error"]["code"], "COMMIT_OUTCOME_UNKNOWN");
    BOOST_CHECK(!body(wire).contains("access_token"));
    f.stop();
    auto r = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(r);
    auto read = r.value().metadata->beginRead();
    BOOST_REQUIRE(read);
    auto user = read.value()->users().findPassword(LoginName::parse("administrator").value());
    BOOST_REQUIRE(user && user.value());
    auto count = read.value()->sessions().countActive(user.value()->userId, r.value().clock->now());
    BOOST_REQUIRE(count);
    BOOST_CHECK_EQUAL(count.value().value(), 1);
}
BOOST_AUTO_TEST_CASE(forwarded_source_controls_real_login_limiter) {
    BrowserFixture f;
    f.start();
    auto data = loginBody("missing_user");
    data["credential_mode"] = "cookie";
    for (int i = 0; i < 3; ++i) status(f.send("POST", "/api/v1/auth/sessions", data, same), 401);
    auto limited = f.send("POST", "/api/v1/auth/sessions", data, same);
    status(limited, 429);
    cleared(limited);
    status(f.send("POST", "/api/v1/auth/sessions", data, same, {}, "203.0.113.2"), 401);
    status(f.send("POST", "/api/v1/auth/sessions", data, same, {}, "203.0.113.2, 203.0.113.1"), 400);
}

BOOST_AUTO_TEST_CASE(cookie_password_unknown_commit_revokes_epoch_without_claiming_rollback) {
    BrowserFixture f;
    f.start();
    auto cookie = f.cookieLogin();
    auto me = f.send("GET", "/api/v1/users/me", nullptr, cookie);
    status(me, 200);
    f.stop();
    f.start({}, true);
    auto changed = f.send("POST", "/api/v1/users/me/password", { { "current_password", "fixture password only" }, { "new_password", "changed fixture password" } }, cookie + same + "If-Match: " + header(me, "etag") + "\r\n");
    status(changed, 500);
    cleared(changed);
    BOOST_CHECK_EQUAL(body(changed)["error"]["code"], "COMMIT_OUTCOME_UNKNOWN");
    status(f.send("GET", "/api/v1/users/me", nullptr, cookie), 401);
    f.stop();
    f.start();
    auto credentials = loginBody();
    credentials["credential_mode"] = "cookie";
    credentials["password"] = "changed fixture password";
    auto loggedIn = f.send("POST", "/api/v1/auth/sessions", credentials, same);
    status(loggedIn, 201);
    status(f.send("GET", "/api/v1/users/me", nullptr, cookieHeader(loggedIn)), 200);
}
BOOST_AUTO_TEST_CASE(cookie_shutdown_waits_for_password_worker_and_does_not_issue_session) {
    BrowserFixture f;
    f.start([&]() -> Result<PasswordWorkerResources> {
        auto r = makeInstancePasswordResources();
        if (!r) return r.error();
        r.value().hasher = std::make_unique<Hasher>(std::move(r.value().hasher), f.gate);
        return std::move(r).value();
    });
    auto data = loginBody();
    data["credential_mode"] = "cookie";
    auto wire = request("POST", "/api/v1/auth/sessions", data, {}, same + "X-Forwarded-For: 203.0.113.1\r\nX-Forwarded-Proto: https\r\n");
    wire.replace(wire.find("Host: localhost"), 15, "Host: cloud.test");
    Client client(*f.server->port());
    client.send(wire);
    BOOST_REQUIRE(drive(f.loop, [&] { return f.gate.entered.load(); }));
    f.server->requestStop();
    BOOST_REQUIRE(drive(f.loop, [&] { client.read(); return client.eof; }));
    f.locked();
    BOOST_CHECK(f.server->state() == RuntimeState::Stopping);
    f.gate.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.server->state() == RuntimeState::Stopped; }));
    BOOST_CHECK_EQUAL(f.server->metadataSnapshot().outstanding, 0);
    BOOST_CHECK(client.bytes.find("access_token") == std::string::npos);
    if (!header(client.bytes, "set-cookie").empty()) cleared(client.bytes);
    f.server.reset();
    auto locks = system::makeLinuxInstanceLockProvider();
    BOOST_REQUIRE(locks);
    BOOST_REQUIRE(locks.value()->acquire(f.root));
    auto r = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(r);
    auto read = r.value().metadata->beginRead();
    BOOST_REQUIRE(read);
    auto user = read.value()->users().findPassword(LoginName::parse("administrator").value());
    BOOST_REQUIRE(user && user.value());
    auto count = read.value()->sessions().countActive(user.value()->userId, r.value().clock->now());
    BOOST_REQUIRE(count);
    BOOST_CHECK_EQUAL(count.value().value(), 0);
}
BOOST_AUTO_TEST_CASE(instance_server_honors_configured_ipv6_loopback) {
    Fixture f(1048576, false, "::1");
    f.start();
    Client client(*f.server->port(), true);
    client.send(request("GET", "/health/ready"));
    BOOST_REQUIRE(drive(f.loop, [&] { client.read(); return client.eof; }));
    status(client.bytes, 200);
    f.stop();
}

BOOST_AUTO_TEST_CASE(cookie_session_has_absolute_expiry_without_request_renewal) {
    BrowserFixture f;
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    f.controlledUtc = std::make_shared<std::atomic<std::int64_t>>(now);
    f.start();
    auto cookie = f.cookieLogin();
    f.controlledUtc->store(now + 7LL * 86400 * 1000000 - 1);
    auto last = f.send("GET", "/api/v1/users/me", nullptr, cookie);
    status(last, 200);
    BOOST_CHECK(header(last, "set-cookie").empty());
    f.controlledUtc->store(now + 7LL * 86400 * 1000000);
    status(f.send("GET", "/api/v1/users/me", nullptr, cookie), 401);
    auto logout = f.send("DELETE", "/api/v1/auth/session", nullptr, cookie + same);
    status(logout, 401);
    cleared(logout);
}
