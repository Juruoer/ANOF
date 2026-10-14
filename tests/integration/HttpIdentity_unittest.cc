#define BOOST_TEST_MODULE HttpIdentityIntegration
#include <boost/test/unit_test.hpp>
#include <tests/integration/InitializedInstance.h>
#include <anof/runtime/InstanceServer.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/application/identity/IdentityService.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <muduox/net/EventLoop.h>
#include <muduox/net/InetAddress.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
using namespace anof;
using namespace anof::runtime;
using namespace anof::infrastructure;
using Json = nlohmann::json;
namespace {
bool drive(muduox::net::EventLoop& loop, const std::function<bool()>& done) {
    if (done()) return true;
    // 截止使用单调时钟，避免系统时间调整使 runAfter 提前判定测试超时。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto timer = loop.runEvery(0.001, [&] { if (done() || std::chrono::steady_clock::now() >= deadline) loop.quit(); });
    loop.loop(); loop.cancel(timer); return done();
}
struct Client {
    int fd;
    std::string bytes;
    bool eof = false;
    explicit Client(std::uint16_t port) : fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        if (fd < 0) throw std::runtime_error("socket failed");
        muduox::net::InetAddress address(port, true);
        if (::connect(fd, address.getSockAddr(), sizeof(sockaddr_in)) != 0) { ::close(fd); throw std::runtime_error("connect failed"); }
    }
    ~Client() { ::close(fd); }
    void send(const std::string& text) {
        BOOST_REQUIRE_EQUAL(::send(fd, text.data(), text.size(), MSG_NOSIGNAL | MSG_DONTWAIT), static_cast<ssize_t>(text.size()));
    }
    void read() {
        char buffer[4096];
        for (;;) {
            auto n = ::recv(fd, buffer, sizeof buffer, MSG_DONTWAIT);
            if (n > 0) { bytes.append(buffer, static_cast<std::size_t>(n)); if (bytes.size() > 512 * 1024) throw std::runtime_error("response limit"); }
            else if (n == 0 || (n < 0 && errno == ECONNRESET)) { eof = true; return; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            else throw std::runtime_error("receive failed");
        }
    }
};
std::string request(std::string method, std::string path, const Json& body = nullptr, std::string token = {}, std::string extra = {}) {
    std::string text = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + extra;
    if (!token.empty()) text += "Authorization: Bearer " + token + "\r\n";
    if (body.is_null()) return text + "\r\n";
    auto json = body.dump();
    return text + "Content-Type: application/json\r\nContent-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
}
Json loginBody(std::string name = "administrator") { return {{"login", name}, {"password", "fixture password only"}, {"credential_mode", "bearer"}}; }
Json registration(std::string name, std::string token) { return {{"login", name}, {"display_name", name}, {"password", "fixture password only"}, {"invite_token", token}}; }
struct Gate {
    std::mutex mutex; std::condition_variable cv;
    std::atomic<bool> entered{false}; bool released = false;
    void release() { { std::lock_guard<std::mutex> lock(mutex); released = true; } cv.notify_all(); }
};
struct Hasher final : PasswordHasher {
    std::unique_ptr<PasswordHasher> inner; Gate& gate;
    Hasher(std::unique_ptr<PasswordHasher> value, Gate& g) : inner(std::move(value)), gate(g) {}
    Result<EncodedPasswordHash> hash(const PasswordInput& input) override { return inner->hash(input); }
    Result<bool> verify(const PasswordInput& input, const EncodedPasswordHash& encoded) override {
        gate.entered = true;
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (!gate.cv.wait_for(lock, std::chrono::seconds(4), [&] { return gate.released; })) return ErrorCode::InternalError;
        lock.unlock(); return inner->verify(input, encoded);
    }
};
// 只对发生身份写入的事务注入“已提交但确认丢失”，清理事务保持真实结果。
struct AuditProbe final : IdentityAuditRepository {
    IdentityAuditRepository& inner; bool wrote = false;
    explicit AuditProbe(IdentityAuditRepository& value) : inner(value) {}
    Result<void> append(const IdentityEvent& event) override { wrote = true; return inner.append(event); }
};
struct UncertainTransaction final : MetadataTransaction {
    std::unique_ptr<MetadataTransaction> inner; AuditProbe audit;
    explicit UncertainTransaction(std::unique_ptr<MetadataTransaction> value) : inner(std::move(value)), audit(inner->identityAudit()) {}
    AuthorizationReader& authorization() override { return inner->authorization(); }
    UserRepository& users() override { return inner->users(); }
    SessionRepository& sessions() override { return inner->sessions(); }
    InviteRepository& invites() override { return inner->invites(); }
    SpaceRepository& spaces() override { return inner->spaces(); }
    NodeRepository& nodes() override { return inner->nodes(); }
    QuotaRepository& quotas() override { return inner->quotas(); }
    SettingsRepository& settings() override { return inner->settings(); }
    AuditRepository& directoryAudit() override { return inner->directoryAudit(); }
    IdentityAuditRepository& identityAudit() override { return audit; }
    Result<void> commit() override {
        auto result = inner->commit();
        return result && audit.wrote ? Result<void>{ErrorCode::CommitOutcomeUnknown} : result;
    }
};
struct UncertainStore final : MetadataStore {
    std::unique_ptr<MetadataStore> inner;
    explicit UncertainStore(std::unique_ptr<MetadataStore> value) : inner(std::move(value)) {}
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override { return inner->beginRead(); }
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override {
        auto result = inner->beginWrite(); if (!result) return result.error();
        return std::unique_ptr<MetadataTransaction>(new UncertainTransaction(std::move(result).value()));
    }
};
struct Fixture {
    std::filesystem::path root;
    muduox::net::EventLoop loop;
    Gate gate;
    std::unique_ptr<InstanceServer> server;
    Fixture() {
        char pattern[] = "/tmp/anof-http-identity-XXXXXX";
        auto* path = ::mkdtemp(pattern); BOOST_REQUIRE(path); root = path;
        auto options = anof::testing::testInitOptions(root); options.configuration.shutdownGraceSeconds = 0;
        BOOST_REQUIRE(initializeInstance(options, PasswordInput::parse("fixture password only").value()));
    }
    ~Fixture() { gate.release(); stop(); std::error_code ec; std::filesystem::remove_all(root, ec); }
    void stop() {
        if (!server) return;
        server->requestStop();
        if (!drive(loop, [&] { return server->state() == RuntimeState::Stopped; })) std::terminate();
        server.reset();
    }
    void start(IdentityExecutor::Factory password = {}, bool uncertain = false) {
        auto locks = system::makeLinuxInstanceLockProvider(); auto config = config::makeTomlInstanceConfigurationLoader();
        auto maintenance = sqlite::makeSqliteInstanceMaintenance({instancePasswordPolicy().verification, {}});
        BOOST_REQUIRE(locks && config && maintenance);
        auto prepared = prepareInstance(root, *locks.value(), *config.value(), *maintenance.value()); BOOST_REQUIRE(prepared);
        if (!password && !uncertain) {
            auto created = makeInstanceServer(loop, std::move(prepared).value()); BOOST_REQUIRE(created); server = std::move(created).value();
        } else {
            auto codec = crypto::makeSodiumSessionTokenCodec(); auto ids = crypto::makeSodiumRequestIdentity(); BOOST_REQUIRE(codec && ids);
            server = std::make_unique<InstanceServer>(loop, std::move(prepared).value(),
                InstanceCrypto{std::move(codec).value(), std::move(ids).value()},
                [this, uncertain]() -> Result<MetadataWorkerResources> {
                    auto resources = makeInstanceMetadataResources(root / ".anof/metadata.sqlite3");
                    if (!resources) return resources.error();
                    if (uncertain) resources.value().metadata = std::make_unique<UncertainStore>(std::move(resources.value().metadata));
                    return std::move(resources).value();
                }, password ? std::move(password) : IdentityExecutor::Factory{makeInstancePasswordResources});
        }
        bool ready = false;
        BOOST_REQUIRE(server->start([&](Result<void> result) { BOOST_REQUIRE(result); ready = true; }));
        BOOST_REQUIRE(drive(loop, [&] { return ready; })); BOOST_REQUIRE(server->ready());
    }
    std::string exchange(const std::string& text) {
        Client client(*server->port()); client.send(text);
        BOOST_REQUIRE(drive(loop, [&] { client.read(); return client.eof; })); return client.bytes;
    }
    Json call(std::string method, std::string path, int status, const Json& body = nullptr, std::string token = {}, std::string extra = {}) {
        auto wire = exchange(request(method, path, body, token, extra));
        BOOST_REQUIRE(wire.find("HTTP/1.1 " + std::to_string(status) + " ") == 0);
        BOOST_CHECK(wire.find("cache-control: no-store\r\n") != std::string::npos);
        BOOST_CHECK(wire.find("x-request-id: ") != std::string::npos);
        auto split = wire.find("\r\n\r\n"); BOOST_REQUIRE(split != std::string::npos);
        auto payload = wire.substr(split + 4);
        if (status == 204) { BOOST_CHECK(payload.empty()); return nullptr; }
        return Json::parse(payload);
    }
    std::string login(std::string name = "administrator") { return call("POST", "/api/v1/auth/sessions", 201, loginBody(name))["access_token"]; }
    void locked() {
        auto locks = system::makeLinuxInstanceLockProvider(); BOOST_REQUIRE(locks);
        auto lock = locks.value()->acquire(root); BOOST_REQUIRE(!lock); BOOST_CHECK(lock.error().code == ErrorCode::Conflict);
    }
};
}
BOOST_AUTO_TEST_CASE(two_users_real_http_invites_spaces_paging_mkdir_logout_and_restart) {
    Fixture f; f.start(); auto admin = f.login();
    auto invite = f.call("POST", "/api/v1/admin/invites", 201, {{"max_uses", 2}}, admin);
    auto first = f.call("POST", "/api/v1/auth/register", 201, registration("member_one", invite["token"]));
    auto second = f.call("POST", "/api/v1/auth/register", 201, registration("member_two", invite["token"]));
    BOOST_CHECK(first["id"] != second["id"]); BOOST_CHECK(!first.contains("access_token"));
    auto one = f.login("member_one"); auto two = f.login("member_two");
    auto spaces = f.call("GET", "/api/v1/spaces", 200, nullptr, one);
    Json privateSpace, publicSpace;
    for (const auto& space : spaces["items"]) (space["kind"] == "private" ? privateSpace : publicSpace) = space;
    BOOST_CHECK_EQUAL(privateSpace["quota"]["used"]["nodes"].get<std::string>(), "1");
    auto parent = privateSpace["root_node_id"].get<std::string>();
    for (const auto& name : {"alpha", "beta"}) f.call("POST", "/api/v1/nodes", 201, {{"space_id", privateSpace["id"]}, {"parent_id", parent}, {"name", name}}, one);
    auto page = f.call("GET", "/api/v1/nodes/" + parent + "/children?limit=1", 200, nullptr, one);
    BOOST_REQUIRE(!page["next_cursor"].is_null());
    auto next = f.call("GET", "/api/v1/nodes/" + parent + "/children?limit=1&cursor=" + page["next_cursor"].get<std::string>(), 200, nullptr, one);
    BOOST_CHECK(page["items"][0]["id"] != next["items"][0]["id"]);
    f.call("GET", "/api/v1/nodes/" + parent, 404, nullptr, two);
    f.call("GET", "/api/v1/nodes/" + parent, 404, nullptr, admin);
    auto pubRoot = publicSpace["root_node_id"].get<std::string>();
    auto misplaced = f.call("GET", "/api/v1/nodes/" + pubRoot + "/children?cursor=" + page["next_cursor"].get<std::string>(), 400, nullptr, one);
    BOOST_CHECK_EQUAL(misplaced["error"]["code"].get<std::string>(), "INVALID_ARGUMENT");
    f.call("POST", "/api/v1/nodes", 201, {{"space_id", publicSpace["id"]}, {"parent_id", pubRoot}, {"name", "shared"}}, one);
    auto shared = f.call("GET", "/api/v1/nodes/" + pubRoot + "/children", 200, nullptr, two);
    BOOST_CHECK_EQUAL(shared["items"].size(), 1);
    f.call("GET", "/api/v1/admin/invites", 403, nullptr, two);
    auto listed = f.call("GET", "/api/v1/admin/invites", 200, nullptr, admin);
    BOOST_CHECK_EQUAL(listed["items"][0]["used_count"].get<int>(), 2); BOOST_CHECK(!listed["items"][0].contains("token"));
    f.call("DELETE", "/api/v1/admin/invites/" + invite["invite"]["id"].get<std::string>(), 204, nullptr, admin);
    f.call("DELETE", "/api/v1/auth/session", 204, nullptr, two);
    f.call("GET", "/api/v1/users/me", 401, nullptr, two);
    f.stop(); f.start();
    auto me = f.call("GET", "/api/v1/users/me", 200, nullptr, one); BOOST_CHECK(me["id"] == first["id"]);
    f.call("GET", "/api/v1/users/me", 401, nullptr, two);
}
BOOST_AUTO_TEST_CASE(invite_consumption_rolls_back_and_revocation_reaches_http) {
    Fixture f; f.start(); auto admin = f.login();
    auto invite = f.call("POST", "/api/v1/admin/invites", 201, Json::object(), admin);
    auto duplicate = f.call("POST", "/api/v1/auth/register", 409, registration("administrator", invite["token"]));
    BOOST_CHECK_EQUAL(duplicate["error"]["code"].get<std::string>(), "LOGIN_CONFLICT");
    f.call("POST", "/api/v1/auth/register", 201, registration("member_one", invite["token"]));
    auto exhausted = f.call("POST", "/api/v1/auth/register", 409, registration("member_two", invite["token"]));
    BOOST_CHECK_EQUAL(exhausted["error"]["code"].get<std::string>(), "INVITE_UNAVAILABLE");
    auto other = f.call("POST", "/api/v1/admin/invites", 201, Json::object(), admin);
    f.call("DELETE", "/api/v1/admin/invites/" + other["invite"]["id"].get<std::string>(), 204, nullptr, admin);
    auto revoked = f.call("POST", "/api/v1/auth/register", 409, registration("member_three", other["token"]));
    BOOST_CHECK_EQUAL(revoked["error"]["code"].get<std::string>(), "INVITE_UNAVAILABLE");
}
BOOST_AUTO_TEST_CASE(real_session_cap_is_409_and_peer_rate_limit_is_429) {
    Fixture f;
    {
        // 使用真实用例建立32个已提交会话，避免以慢速／压力循环绕开在线限流。
        auto r = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3"); auto p = makeInstancePasswordResources(); BOOST_REQUIRE(r && p);
        IdentityService service(*r.value().metadata, *r.value().clock, *r.value().identity->ids, *r.value().identity->issuer, *r.value().identity->invites);
        LoginCommand command{LoginName::parse("administrator").value(), PasswordInput::parse("fixture password only").value(), CredentialMode::Bearer, AuthenticationSource::ipv4({127,0,0,1})};
        auto snapshot = service.prepareLogin(command.login); BOOST_REQUIRE(snapshot && snapshot.value());
        auto verified = p.value().hasher->verify(command.password, snapshot.value()->encodedHash); BOOST_REQUIRE(verified && verified.value());
        auto id = RequestId::parse("12345678-1234-4234-8234-123456789abc").value();
        for (int i = 0; i < 32; ++i) BOOST_REQUIRE(service.completeLogin(id, command, snapshot.value(), verified.value()));
    }
    f.start();
    for (int i = 0; i < 3; ++i) {
        auto cap = f.call("POST", "/api/v1/auth/sessions", 409, loginBody());
        BOOST_CHECK_EQUAL(cap["error"]["code"].get<std::string>(), "SESSION_LIMIT_REACHED");
    }
    auto limited = f.call("POST", "/api/v1/auth/sessions", 429, loginBody(), {}, "X-Forwarded-For: 203.0.113.9\r\n");
    BOOST_CHECK_EQUAL(limited["error"]["code"].get<std::string>(), "RATE_LIMITED");
    BOOST_CHECK(!limited.contains("access_token"));
}
BOOST_AUTO_TEST_CASE(http_cleanup_waits_for_password_exit_and_keeps_real_lease) {
    Fixture f;
    f.start([&]() -> Result<PasswordWorkerResources> {
        auto resources = makeInstancePasswordResources(); if (!resources) return resources.error();
        resources.value().hasher = std::make_unique<Hasher>(std::move(resources.value().hasher), f.gate);
        return std::move(resources).value();
    });
    Client client(*f.server->port()); client.send(request("POST", "/api/v1/auth/sessions", loginBody()));
    BOOST_REQUIRE(drive(f.loop, [&] { return f.gate.entered.load(); }));
    // 密码运行期间真实 HTTP / metadata 请求仍有进展。
    f.call("GET", "/api/v1/users/me", 401, nullptr, std::string(43, 'A'));
    f.server->requestStop(); f.server->requestStop();
    BOOST_REQUIRE(drive(f.loop, [&] { client.read(); return client.eof; }));
    BOOST_CHECK(f.server->state() == RuntimeState::Stopping); f.locked();
    f.gate.release(); BOOST_REQUIRE(drive(f.loop, [&] { return f.server->state() == RuntimeState::Stopped; }));
    BOOST_CHECK_EQUAL(f.server->metadataSnapshot().outstanding, 0); f.locked();
    BOOST_CHECK(client.bytes.find("access_token") == std::string::npos);
    f.server.reset();
    auto resources = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3"); BOOST_REQUIRE(resources);
    auto read = resources.value().metadata->beginRead(); BOOST_REQUIRE(read);
    auto user = read.value()->users().findPassword(LoginName::parse("administrator").value()); BOOST_REQUIRE(user && user.value());
    auto count = read.value()->sessions().countActive(user.value()->userId, resources.value().clock->now());
    BOOST_REQUIRE(count); BOOST_CHECK_EQUAL(count.value().value(), 0);
    read.value().reset(); resources.value().metadata.reset();
    auto locks = system::makeLinuxInstanceLockProvider(); BOOST_REQUIRE(locks); BOOST_REQUIRE(locks.value()->acquire(f.root));
}

BOOST_AUTO_TEST_CASE(committed_login_with_unknown_confirmation_returns_no_token_and_is_not_retried) {
    Fixture f; f.start({}, true);
    auto result = f.call("POST", "/api/v1/auth/sessions", 500, loginBody());
    BOOST_CHECK_EQUAL(result["error"]["code"].get<std::string>(), "COMMIT_OUTCOME_UNKNOWN");
    BOOST_CHECK(!result.contains("access_token"));
    f.stop();
    auto r = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3"); BOOST_REQUIRE(r);
    auto read = r.value().metadata->beginRead(); BOOST_REQUIRE(read);
    auto user = read.value()->users().findPassword(LoginName::parse("administrator").value()); BOOST_REQUIRE(user && user.value());
    auto sessions = read.value()->sessions().countActive(user.value()->userId, r.value().clock->now());
    BOOST_REQUIRE(sessions); BOOST_CHECK_EQUAL(sessions.value().value(), 1);
}

namespace {
std::string condition(const Json& user) { return "If-Match: \"user-"+user["id"].get<std::string>()+"-"+user["revision"].get<std::string>()+"\"\r\n"; }
Json passwordChange() { return {{"current_password","fixture password only"},{"new_password","new fixture password"}}; }
}
BOOST_AUTO_TEST_CASE(account_http_password_gate_rechecks_revision_and_shutdown_keeps_lease) {
    Fixture f;f.start();const auto token=f.login();auto user=f.call("GET","/api/v1/users/me",200,nullptr,token);f.stop();
    f.start([&]() -> Result<PasswordWorkerResources> {
        auto resources=makeInstancePasswordResources();if(!resources)return resources.error();
        resources.value().hasher=std::make_unique<Hasher>(std::move(resources.value().hasher),f.gate);return std::move(resources).value();
    });
    Client change(*f.server->port());change.send(request("POST","/api/v1/users/me/password",passwordChange(),token,condition(user)));
    BOOST_REQUIRE(drive(f.loop,[&]{return f.gate.entered.load();}));
    // 同一真实 metadata worker 在改密计算被门控时仍可提交资料更新。
    auto renamed=f.call("PATCH","/api/v1/users/me",200,{{"display_name","renamed"}},token,condition(user));
    f.gate.release();BOOST_REQUIRE(drive(f.loop,[&]{change.read();return change.eof;}));
    BOOST_CHECK(change.bytes.find("HTTP/1.1 412 ")==0);BOOST_CHECK(change.bytes.find("PRECONDITION_FAILED")!=std::string::npos);
    f.call("GET","/api/v1/users/me",200,nullptr,token);
    f.gate.entered=false;{std::lock_guard<std::mutex> lock(f.gate.mutex);f.gate.released=false;}
    Client pending(*f.server->port());pending.send(request("POST","/api/v1/users/me/password",passwordChange(),token,condition(renamed)));
    BOOST_REQUIRE(drive(f.loop,[&]{return f.gate.entered.load();}));
    f.server->requestStop();BOOST_REQUIRE(drive(f.loop,[&]{pending.read();return pending.eof;}));
    BOOST_CHECK(f.server->state()==RuntimeState::Stopping);f.locked();
    f.gate.release();BOOST_REQUIRE(drive(f.loop,[&]{return f.server->state()==RuntimeState::Stopped;}));f.locked();
    f.server.reset();f.start();f.call("GET","/api/v1/users/me",200,nullptr,token);
}
BOOST_AUTO_TEST_CASE(account_http_authorization_before_binding_and_unknown_password_commit) {
    Fixture f;f.start();const auto admin=f.login();
    auto invite=f.call("POST","/api/v1/admin/invites",201,Json::object(),admin)["token"].get<std::string>();
    auto user=f.call("POST","/api/v1/auth/register",201,registration("alice",invite));const auto alice=f.login("alice");
    const auto other="10000000-0000-4000-8000-000000000099";
    auto rejected=f.call("PATCH",std::string("/api/v1/admin/users/")+other,403,{{"enabled",false}},alice,condition(user));
    BOOST_CHECK_EQUAL(rejected["error"]["code"].get<std::string>(),"FORBIDDEN");
    f.call("PATCH",std::string("/api/v1/admin/users/")+other,404,{{"enabled",false}},admin,condition(user));
    auto wrong="If-Match: \"quota-space-10000000-0000-4000-8000-000000000099-1\"\r\n";
    f.call("PATCH","/api/v1/users/me",412,{{"display_name","unused"}},alice,wrong);
    auto before=f.call("GET","/api/v1/users/me",200,nullptr,alice);f.stop();f.start({},true);
    auto uncertain=f.call("POST","/api/v1/users/me/password",500,passwordChange(),alice,condition(before));
    BOOST_CHECK_EQUAL(uncertain["error"]["code"].get<std::string>(),"COMMIT_OUTCOME_UNKNOWN");
    f.call("GET","/api/v1/users/me",401,nullptr,alice);f.stop();f.start();
    auto login=loginBody("alice");login["password"]="new fixture password";
    auto confirmed=f.call("POST","/api/v1/auth/sessions",201,login);BOOST_CHECK(confirmed.contains("access_token"));
    auto after=f.call("GET","/api/v1/users/me",200,nullptr,confirmed["access_token"].get<std::string>());
    BOOST_CHECK_EQUAL(std::stoll(after["revision"].get<std::string>()),std::stoll(before["revision"].get<std::string>())+1);
}
