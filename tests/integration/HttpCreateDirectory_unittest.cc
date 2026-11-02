#include <tests/support/EventLoopWait.h>
#include <anof/runtime/IdentityExecutor.h>
#include <anof/runtime/DriveExecutor.h>
#include <anof/runtime/UploadExecutor.h>
#define BOOST_TEST_MODULE AnofHttpCreateDirectoryIntegration
#include <boost/test/unit_test.hpp>

#include <anof/infrastructure/crypto/SodiumNodeIdGenerator.h>
#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/system/SystemClock.h>
#include <anof/runtime/AnofRuntime.h>
#include <anof/transport/http/CreateDirectoryHandler.h>
#include <anof/transport/http/JsonResponse.h>
#include <tests/persistence_contract/TestMetadataBackend.h>
#include <muduox/net/EventLoop.h>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace anof::testing {
namespace {
using namespace anof::runtime;
using namespace anof::http;
using namespace muduox::net;
using namespace anof::infrastructure::crypto;

template <typename T> T id(unsigned number) {
    char text[37];
    std::snprintf(text, sizeof text, "00000000-0000-4000-8000-%012x", number);
    return T::parse(text).value();
}
QuotaDelta amount(std::int64_t nodes) { return { ByteCount{}, NodeCount::from(nodes).value() }; }

bool drive(muduox::net::EventLoop& loop, const std::function<bool()>& done,
           const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
    return anof::test::waitFor(loop, done, 3, file, line);
}

struct Client {
    int fd;
    std::string bytes;
    bool eof = false;
    explicit Client(const InetAddress& address) : fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        if (fd < 0) throw std::runtime_error("socket failed");
        if (::connect(fd, address.getSockAddr(), sizeof(sockaddr_in)) != 0) {
            ::close(fd);
            throw std::runtime_error("connect failed");
        }
    }
    Client(const Client&) = delete;
    ~Client() { ::close(fd); }
    void send(const std::string& text) {
        const auto n = ::send(fd, text.data(), text.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        // 测试请求只有数百字节；短写立即失败，不在 loop 上阻塞重试。
        BOOST_REQUIRE_EQUAL(n, static_cast<ssize_t>(text.size()));
    }
    void read() {
        char buffer[4096];
        for (;;) {
            const auto n = ::recv(fd, buffer, sizeof buffer, MSG_DONTWAIT);
            if (n > 0) {
                bytes.append(buffer, static_cast<std::size_t>(n));
                if (bytes.size() > 65536) throw std::runtime_error("response fixture exceeded bound");
            } else if (n == 0 || errno == ECONNRESET) { eof = true; return; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            else throw std::runtime_error("recv failed");
        }
    }
};

enum class Fault { None, Audit, UnknownRollback, UnknownCommit, ExpireSecondCheck };
struct Control {
    Fault fault = Fault::None;
    std::atomic<int> begins{0}, auths{0}, commits{0};
    std::mutex mutex;
    std::condition_variable changed;
    bool blocked = false;
    bool released = false;
    std::atomic<bool> entered{false};
    std::thread::id opened, closed;
    void release() {
        { std::lock_guard<std::mutex> lock(mutex); released = true; }
        changed.notify_all();
    }
};
// 仅装饰类型化端口；所有写入、鉴权、提交及回滚仍由真实 SQLite 执行。
class Transaction final : public MetadataTransaction, public AuthorizationReader, public AuditRepository {
public:
    Transaction(std::unique_ptr<MetadataTransaction> inner, Control& control)
        : inner_(std::move(inner)), control_(control) {}
    AuthorizationReader& authorization() override { return *this; }
    UserRepository& users() override { return inner_->users(); }
    SessionRepository& sessions() override { return inner_->sessions(); }
    InviteRepository& invites() override { return inner_->invites(); }
    SettingsRepository& settings() override { return inner_->settings(); }
    IdentityAuditRepository& identityAudit() override { return inner_->identityAudit(); }
    NodeAccountingRepository& nodeAccounting() override { return inner_->nodeAccounting(); }

    SpaceRepository& spaces() override { return inner_->spaces(); }
    NodeRepository& nodes() override { return inner_->nodes(); }
    QuotaRepository& quotas() override { return inner_->quotas(); }
    AuditRepository& directoryAudit() override { return *this; }
    Result<Principal> requireActiveSession(const SessionCredential& digest, UtcTime now) override {
        const auto count = ++control_.auths;
        if (control_.fault == Fault::ExpireSecondCheck && count == 2) now += std::chrono::hours(2);
        return inner_->authorization().requireActiveSession(digest, now);
    }
    Result<void> append(const DirectoryCreatedEvent& event) override {
        auto result = inner_->directoryAudit().append(event);
        if (result && control_.fault == Fault::Audit) return ErrorCode::InternalError;
        return result;
    }
    Result<void> commit() override {
        ++control_.commits;
        if (control_.fault == Fault::UnknownRollback) {
            inner_.reset();
            return ErrorCode::CommitOutcomeUnknown;
        }
        auto result = inner_->commit();
        if (result && control_.fault == Fault::UnknownCommit) return ErrorCode::CommitOutcomeUnknown;
        return result;
    }
private:
    std::unique_ptr<MetadataTransaction> inner_;
    Control& control_;
};
class Store final : public MetadataStore {
public:
    Store(std::unique_ptr<MetadataStore> inner, Control& control) : inner_(std::move(inner)), control_(control) {
        control_.opened = std::this_thread::get_id();
    }
    ~Store() override { inner_.reset(); control_.closed = std::this_thread::get_id(); }
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override { return inner_->beginRead(); }
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override {
        ++control_.begins;
        // gate 在事务开启之前；用于确定 Running/Queued 次序，不占用 SQLite 写锁。
        if (control_.blocked) {
            std::unique_lock<std::mutex> lock(control_.mutex);
            control_.entered = true;
            if (!control_.changed.wait_for(lock, std::chrono::seconds(5), [&] { return control_.released; }))
                return ErrorCode::InternalError;
        }
        auto tx = inner_->beginWrite();
        if (!tx) return tx.error();
        return std::unique_ptr<MetadataTransaction>(new Transaction(std::move(tx).value(), control_));
    }
private:
    std::unique_ptr<MetadataStore> inner_;
    Control& control_;
};

template <typename T> void record(std::vector<T>& sink, T value) {
    if (sink.size() == 64) throw std::runtime_error("observation fixture exceeded bound");
    sink.push_back(std::move(value));
}
struct Fixture {
    EventLoop loop;
    Control control;
    std::unique_ptr<TestMetadataBackend> backend = makeTestMetadataBackend();
    std::unique_ptr<SessionTokenCodec> codec = std::move(makeSodiumSessionTokenCodec()).value();
    std::unique_ptr<RequestIdentity> identity = std::move(makeSodiumRequestIdentity()).value();
    UserId owner = id<UserId>(1), admin = id<UserId>(2);
    SpaceId space = id<SpaceId>(1), publicSpace = id<SpaceId>(2);
    NodeId root = id<NodeId>(1), publicRoot = id<NodeId>(2);
    std::string token, adminToken;
    MetadataSeed seed;
    std::vector<DirectoryTaskRecord> tasks;
    std::vector<HttpTraceRecord> finished;
    AnofRuntime runtime{loop, {1, 1}};
    // 本夹具只验收目录；身份 dispatcher 不启动，身份端点由完整实例测试覆盖。
    IdentityExecutor identities{loop, runtime.metadataExecutor()};
    DriveExecutor files{loop, runtime.metadataExecutor()};
    DiskExecutor disk{loop};
    UploadExecutor uploads{loop, runtime.metadataExecutor(), disk, DiskSafetyPolicy{}};
    CreateDirectoryHandler handler{runtime.dispatcher(), [this] { return runtime.ready(); },
        [this](auto value) { record(tasks, std::move(value)); }};
    std::unique_ptr<HttpApi> api;
    HttpServer server{&loop, InetAddress(0, true), "anof-e2e"};
    bool started = false, stopping = false, httpStopped = false;

    std::string newToken(std::optional<SessionDigest>* digest = nullptr) {
        std::array<unsigned char, 32> raw{};
        randombytes_buf(raw.data(), raw.size());
        SessionDigest::Bytes hashed{};
        if (crypto_hash_sha256(hashed.data(), raw.data(), raw.size()) != 0)
            throw std::runtime_error("fixture digest failed");
        if (digest) *digest = SessionDigest{ hashed };
        char encoded[44];
        sodium_bin2base64(encoded, sizeof encoded, raw.data(), raw.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
        sodium_memzero(raw.data(), raw.size());
        return encoded;
    }
    Fixture() {
        std::optional<SessionDigest> ownerDigest, adminDigest;
        // 播种直接哈希随机原始字节，避免用被测 codec 的输出自证摘要匹配。
        token = newToken(&ownerDigest);
        adminToken = newToken(&adminDigest);
        const auto now = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
        seed.users = {{{owner, UserRole::Member, AuthEpoch{}}, true}, {{admin, UserRole::Administrator, AuthEpoch{}}, true}};
        seed.sessions = {{*ownerDigest, owner, AuthEpoch{}, now + std::chrono::hours(1), false},
                         {*adminDigest, admin, AuthEpoch{}, now + std::chrono::hours(1), false}};
        seed.spaces = {{space, SpaceKind::Private, owner, root}, {publicSpace, SpaceKind::Public, std::nullopt, publicRoot}};
        seed.nodes = {{{root, space, std::nullopt, "", owner, Revision{}, now, now}, std::nullopt, false},
                      {{publicRoot, publicSpace, std::nullopt, "", admin, Revision{}, now, now}, std::nullopt, false}};
        seed.quotas = {{SpaceQuota{space}, {amount(10), amount(1), amount(0)}},
                       {SpaceQuota{publicSpace}, {amount(10), amount(1), amount(0)}},
                       {PublicContributionQuota{publicSpace, owner}, {amount(10), amount(0), amount(0)}},
                       {PublicContributionQuota{publicSpace, admin}, {amount(10), amount(0), amount(0)}}};
    }
    void start(double responseSeconds = 30) {
        BOOST_REQUIRE(backend->reset(seed));
        api = std::make_unique<HttpApi>(*codec, *identity, [this] { return runtime.ready(); }, handler.stream(), identities, uploads, files, [] { InstanceConfiguration c; c.developmentHttp = true; return c; }(),
            HttpObservation{{}, [this](auto value) { record(finished, std::move(value)); }}, HttpApiOptions{4, 15, responseSeconds});
        api->configure(server);
        BOOST_REQUIRE(runtime.start([this]() -> Result<MetadataWorkerResources> {
            auto store = backend->connect();
            if (!store) return store.error();
            auto clock = infrastructure::system::makeSystemClock();
            if (!clock) return clock.error();
            auto ids = makeSodiumNodeIdGenerator();
            if (!ids) return ids.error();
            return MetadataWorkerResources{std::make_unique<Store>(std::move(store).value(), control),
                                           std::move(clock).value(), std::move(ids).value()};
        }));
        BOOST_REQUIRE(drive(loop, [&] { return runtime.metadataSnapshot().state == MetadataExecutorState::Ready; }));
        BOOST_CHECK(!runtime.ready());
        server.start(); started = true;
        BOOST_REQUIRE(runtime.markHttpStarted());
    }
    void stop(double grace = 0) {
        if (stopping) return;
        stopping = true;
        runtime.requestStop();
        if (started) server.stop(grace, [this] { httpStopped = true; BOOST_CHECK(runtime.markHttpStopped()); });
    }
    void finish() {
        control.release(); stop();
        BOOST_REQUIRE(drive(loop, [&] { return runtime.state() == RuntimeState::Stopped; }));
        BOOST_CHECK_EQUAL(runtime.metadataSnapshot().outstanding, 0U);
        BOOST_CHECK_EQUAL(runtime.metadataSnapshot().callbackExceptions, 0U);
        BOOST_CHECK_EQUAL(runtime.notificationExceptions(), 0U);
        BOOST_CHECK(control.opened != std::this_thread::get_id());
        BOOST_CHECK(control.opened == control.closed);
    }
    ~Fixture() {
        control.release(); stop();
        if (!drive(loop, [&] { return runtime.state() == RuntimeState::Stopped; })) std::terminate();
    }
    std::string post(const std::string& name, const std::string& bearer, bool pub = false) const {
        const auto body = nlohmann::json{{"space_id", (pub ? publicSpace : space).toString()},
            {"parent_id", (pub ? publicRoot : root).toString()}, {"name", name}}.dump();
        return "POST /api/v1/nodes HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: application/json\r\n"
            "X-Request-ID: untrusted\r\nAuthorization: Bearer " + bearer + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    std::string exchange(const std::string& request) {
        Client client(server.listenAddress()); client.send(request);
        BOOST_REQUIRE(drive(loop, [&] { client.read(); return client.eof; }));
        return client.bytes;
    }
    void durable(std::size_t count) {
        auto audit = backend->readAudit(); BOOST_REQUIRE(audit);
        BOOST_REQUIRE_EQUAL(audit.value().size(), count);
        for (const auto& event : audit.value()) {
            bool matched = false;
            for (const auto& task : tasks) if (task.requestId == event.requestId) matched = true;
            BOOST_CHECK(matched);
        }
    }
};
void status(const std::string& response, int code) {
    BOOST_CHECK(response.find("HTTP/1.1 " + std::to_string(code) + " ") == 0);
}
nlohmann::json body(const std::string& response) { return nlohmann::json::parse(response.substr(response.find("\r\n\r\n") + 4)); }
void error(const std::string& response, int code, const char* name) {
    status(response, code); BOOST_CHECK_EQUAL(body(response)["error"]["code"].get<std::string>(), name);
}

BOOST_AUTO_TEST_CASE(real_credentials_success_identity_and_duplicate_are_durable) {
    Fixture f; f.start();
    status(f.exchange("GET /health/ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"), 200);
    const auto response = f.exchange(f.post("家庭", f.token)); status(response, 201);
    const auto value = body(response);
    const auto node = NodeId::parse(value["id"].get<std::string>()).value();
    BOOST_CHECK(response.find("location: /api/v1/nodes/" + node.toString() + "\r\n") != std::string::npos);
    BOOST_CHECK(response.find("etag: \"node-" + node.toString() + "-1\"\r\n") != std::string::npos);
    BOOST_REQUIRE_EQUAL(f.tasks.size(), 1U);
    BOOST_CHECK(response.find("x-request-id: " + f.tasks[0].requestId.toString() + "\r\n") != std::string::npos);
    BOOST_CHECK(f.finished.back().requestId == f.tasks[0].requestId);
    BOOST_REQUIRE(f.tasks[0].attempt);
    BOOST_CHECK(f.tasks[0].attempt->candidateNodeId == node);
    BOOST_CHECK_EQUAL(value["created_by"].get<std::string>(), f.owner.toString());
    BOOST_CHECK_EQUAL(value["created_at"], value["updated_at"]);
    BOOST_CHECK(!f.tasks[0].error);
    error(f.exchange(f.post("家庭", f.token)), 409, "NAME_CONFLICT");
    f.finish(); f.durable(1);
    BOOST_CHECK_EQUAL(f.control.begins.load(), 2);
    BOOST_CHECK_EQUAL(f.control.commits.load(), 1);
    const auto persisted = f.backend->readNode(node).value()->metadata;
    BOOST_CHECK_EQUAL(persisted.name, "家庭");
    const auto audit = f.backend->readAudit().value();
    BOOST_CHECK(audit[0].nodeId == node);
    BOOST_CHECK(audit[0].actor == f.owner);
    BOOST_CHECK(audit[0].spaceId == f.space);
    BOOST_CHECK(audit[0].parentId == f.root);
    // 审计使用第二次鉴权时刻；目录时间来自首次鉴权，不要求两者相等。
    BOOST_CHECK_EQUAL(value["created_at"].get<std::string>(), formatUtcTimestamp(persisted.createdAt).value());
    BOOST_CHECK(persisted.createdAt == persisted.updatedAt);
    BOOST_CHECK_EQUAL(f.backend->readNode(f.root).value()->metadata.revision.value(), 2);
    BOOST_CHECK_EQUAL(f.backend->readQuota(SpaceQuota{f.space}).value().used.nodes.value(), 2);
}

BOOST_AUTO_TEST_CASE(session_state_and_private_isolation) {
    for (int scenario = 0; scenario < 6; ++scenario) {
        Fixture f;
        if (scenario == 1) f.seed.sessions[0].revoked = true;
        if (scenario == 2) f.seed.sessions[0].expiresAt -= std::chrono::hours(2);
        if (scenario == 3) f.seed.users[0].enabled = false;
        if (scenario == 4) f.seed.users[0].principal.authEpoch = AuthEpoch::from(1).value();
        const auto bearer = scenario == 0 ? f.newToken() : scenario == 5 ? f.adminToken : f.token;
        f.start();
        error(f.exchange(f.post("denied", bearer)), scenario == 5 ? 404 : 401, scenario == 5 ? "NOT_FOUND" : "UNAUTHENTICATED");
        f.finish(); f.durable(0);
        BOOST_CHECK_EQUAL(f.control.begins.load(), 1);
        BOOST_CHECK_EQUAL(f.control.commits.load(), 0);
        BOOST_CHECK_EQUAL(f.backend->readQuota(SpaceQuota{f.space}).value().used.nodes.value(), 1);
    }
}

BOOST_AUTO_TEST_CASE(public_quota_charges_both_scopes_and_rolls_back_on_second_failure) {
    for (bool full : {false, true}) {
        Fixture f;
        if (full) f.seed.quotas[2].state.limit = amount(0);
        f.start();
        const auto response = f.exchange(f.post("shared", f.token, true));
        if (full) error(response, 403, "QUOTA_EXCEEDED"); else status(response, 201);
        f.finish(); f.durable(full ? 0 : 1);
        BOOST_CHECK_EQUAL(f.backend->readQuota(SpaceQuota{f.publicSpace}).value().used.nodes.value(), full ? 1 : 2);
        BOOST_CHECK_EQUAL(f.backend->readQuota(PublicContributionQuota{f.publicSpace, f.owner}).value().used.nodes.value(), full ? 0 : 1);
        BOOST_CHECK_EQUAL(f.backend->readNode(f.publicRoot).value()->metadata.revision.value(), full ? 1 : 2);
    }
}

BOOST_AUTO_TEST_CASE(invalid_token_is_rejected_before_dispatch_and_admin_has_no_quota_bypass) {
    Fixture f;
    f.seed.quotas[3].state.limit = amount(0);
    f.start();
    error(f.exchange(f.post("bad-token", "invalid")), 401, "UNAUTHENTICATED");
    BOOST_CHECK_EQUAL(f.control.begins.load(), 0);
    BOOST_CHECK(f.tasks.empty());
    error(f.exchange(f.post("admin-quota", f.adminToken, true)), 403, "QUOTA_EXCEEDED");
    f.finish(); f.durable(0);
    BOOST_CHECK_EQUAL(f.control.begins.load(), 1);
    BOOST_CHECK_EQUAL(f.backend->readQuota(SpaceQuota{f.publicSpace}).value().used.nodes.value(), 1);
    BOOST_CHECK_EQUAL(f.backend->readQuota(PublicContributionQuota{f.publicSpace, f.admin}).value().used.nodes.value(), 0);
}

BOOST_AUTO_TEST_CASE(second_authorization_and_audit_failure_rollback_every_write) {
    for (auto fault : {Fault::ExpireSecondCheck, Fault::Audit}) {
        Fixture f; f.control.fault = fault; f.start();
        const bool expired = fault == Fault::ExpireSecondCheck;
        error(f.exchange(f.post("rollback", f.token)), expired ? 401 : 500, expired ? "UNAUTHENTICATED" : "INTERNAL_ERROR");
        f.finish(); f.durable(0);
        BOOST_CHECK_EQUAL(f.control.auths.load(), 2);
        BOOST_CHECK_EQUAL(f.control.commits.load(), 0);
        BOOST_CHECK(!f.backend->readNode(f.tasks[0].attempt->candidateNodeId).value());
        BOOST_CHECK_EQUAL(f.backend->readNode(f.root).value()->metadata.revision.value(), 1);
        BOOST_CHECK_EQUAL(f.backend->readQuota(SpaceQuota{f.space}).value().used.nodes.value(), 1);
    }
}

BOOST_AUTO_TEST_CASE(unknown_commit_is_not_retried_and_wire_does_not_claim_rollback) {
    for (auto fault : {Fault::UnknownRollback, Fault::UnknownCommit}) {
        Fixture f; f.control.fault = fault; f.start();
        error(f.exchange(f.post("uncertain", f.token)), 500, "COMMIT_OUTCOME_UNKNOWN");
        f.finish();
        const bool committed = fault == Fault::UnknownCommit;
        f.durable(committed ? 1 : 0);
        BOOST_CHECK_EQUAL(f.control.begins.load(), 1);
        BOOST_CHECK_EQUAL(f.control.commits.load(), 1);
        BOOST_CHECK_EQUAL(bool(f.backend->readNode(f.tasks[0].attempt->candidateNodeId).value()), committed);
        BOOST_CHECK(f.tasks[0].error == ErrorCode::CommitOutcomeUnknown);
    }
}

BOOST_AUTO_TEST_CASE(response_timeout_can_precede_real_commit_and_late_completion) {
    Fixture f; f.control.blocked = true; f.start(0.1);
    Client client(f.server.listenAddress()); client.send(f.post("late", f.token));
    BOOST_REQUIRE(drive(f.loop, [&] { return f.control.entered.load(); }));
    BOOST_REQUIRE(drive(f.loop, [&] { client.read(); return client.eof && !f.finished.empty(); }));
    status(client.bytes, 500);
    BOOST_CHECK(f.tasks.empty());
    f.control.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.tasks.size() == 1; }));
    BOOST_CHECK(!f.tasks[0].exchangeActive);
    BOOST_CHECK(!f.tasks[0].error);
    BOOST_CHECK(!f.tasks[0].cancelledBeforeStart);
    BOOST_CHECK_EQUAL(f.finished.size(), 1U);
    f.finish(); f.durable(1);
}

BOOST_AUTO_TEST_CASE(full_queue_stop_cancels_queued_and_http_barrier_does_not_stop_worker) {
    Fixture f; f.control.blocked = true; f.start();
    Client running(f.server.listenAddress()); running.send(f.post("running", f.token));
    BOOST_REQUIRE(drive(f.loop, [&] { return f.control.entered.load(); }));
    Client queued(f.server.listenAddress()); queued.send(f.post("queued", f.token));
    BOOST_REQUIRE(drive(f.loop, [&] { return f.runtime.metadataSnapshot().pending == 1; }));
    error(f.exchange(f.post("overload", f.token)), 503, "TEMPORARILY_UNAVAILABLE");
    BOOST_CHECK_EQUAL(f.runtime.metadataSnapshot().outstanding, 2U);
    f.stop();
    BOOST_CHECK(!f.runtime.ready());
    BOOST_REQUIRE(drive(f.loop, [&] { return f.httpStopped; }));
    BOOST_CHECK(f.runtime.state() == RuntimeState::Stopping);
    BOOST_CHECK(f.tasks.empty());
    f.finish(); f.durable(1);
    BOOST_REQUIRE_EQUAL(f.tasks.size(), 2U);
    unsigned cancelled = 0;
    for (const auto& task : f.tasks) {
        BOOST_CHECK(!task.exchangeActive);
        if (task.cancelledBeforeStart) { ++cancelled; BOOST_CHECK(!task.attempt); }
    }
    BOOST_CHECK_EQUAL(cancelled, 1U);
    BOOST_CHECK_EQUAL(f.control.begins.load(), 1);
}

BOOST_AUTO_TEST_CASE(real_commits_fill_completion_capacity_until_loop_drains) {
    Fixture f; f.start();
    const auto digest = f.seed.sessions[0].digest;
    std::vector<CreateDirectoryCompletion> results;
    const auto submit = [&](unsigned number) {
        return f.runtime.dispatcher().trySubmit({{digest, CredentialMode::Bearer}, id<RequestId>(number)},
            {f.space, f.root, "backlog-" + std::to_string(number)},
            [&](auto result) { results.push_back(std::move(result)); });
    };
    // 暂不运行 loop，确定性保留已提交结果；按快照等待，不用固定延时猜测 worker 进度。
    const auto waitCompleted = [&](std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (f.runtime.metadataSnapshot().completed != count) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    };
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submit(10)));
    BOOST_REQUIRE(waitCompleted(1));
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submit(11)));
    BOOST_REQUIRE(waitCompleted(2));
    const auto snapshot = f.runtime.metadataSnapshot();
    BOOST_CHECK_EQUAL(snapshot.pending, 0U);
    BOOST_CHECK(!snapshot.running);
    BOOST_CHECK_EQUAL(snapshot.outstanding, 2U);
    BOOST_CHECK(results.empty());
    const auto rejected = submit(12);
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(rejected));
    BOOST_CHECK(std::get<SubmitError>(rejected) == SubmitError::QueueFull);
    // 停止不丢弃已经提交的结果；名额直到 loop 回调完成才释放。
    f.stop();
    BOOST_CHECK(f.runtime.state() == RuntimeState::Stopping);
    f.finish();
    BOOST_REQUIRE_EQUAL(results.size(), 2U);
    for (const auto& result : results) {
        BOOST_REQUIRE(std::holds_alternative<Result<CreateDirectoryResult>>(result.outcome));
        BOOST_CHECK(std::get<Result<CreateDirectoryResult>>(result.outcome));
    }
    BOOST_CHECK(results[0].taskId != results[1].taskId);
    BOOST_CHECK_EQUAL(f.control.begins.load(), 2);
    BOOST_CHECK_EQUAL(f.control.commits.load(), 2);
    BOOST_CHECK_EQUAL(f.backend->readAudit().value().size(), 2U);
    BOOST_CHECK_EQUAL(f.backend->readQuota(SpaceQuota{f.space}).value().used.nodes.value(), 3);
    BOOST_CHECK_EQUAL(f.backend->readNode(f.root).value()->metadata.revision.value(), 3);
}

BOOST_AUTO_TEST_CASE(retry_after_timeout_and_late_commit_is_a_new_conflicting_request) {
    Fixture f; f.control.blocked = true; f.start(0.1);
    const auto request = f.post("retry-after-timeout", f.token);
    Client first(f.server.listenAddress()); first.send(request);
    BOOST_REQUIRE(drive(f.loop, [&] { return f.control.entered.load(); }));
    BOOST_REQUIRE(drive(f.loop, [&] { first.read(); return first.eof && f.finished.size() == 1; }));
    status(first.bytes, 500);
    f.control.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.tasks.size() == 1; }));
    BOOST_CHECK(!f.tasks[0].exchangeActive);
    BOOST_REQUIRE(!f.tasks[0].error);
    BOOST_REQUIRE(f.tasks[0].attempt);
    const auto committed = f.tasks[0].attempt->candidateNodeId;
    // 相同报文（包括客户端 request ID）不构成幂等键。
    error(f.exchange(request), 409, "NAME_CONFLICT");
    f.finish(); f.durable(1);
    BOOST_REQUIRE_EQUAL(f.tasks.size(), 2U);
    BOOST_CHECK(f.tasks[0].requestId != f.tasks[1].requestId);
    BOOST_CHECK_EQUAL(f.finished.size(), 2U);
    BOOST_CHECK_EQUAL(f.control.begins.load(), 2);
    BOOST_CHECK_EQUAL(f.control.commits.load(), 1);
    BOOST_REQUIRE(f.backend->readNode(committed).value());
    BOOST_CHECK_EQUAL(f.backend->readQuota(SpaceQuota{f.space}).value().used.nodes.value(), 2);
    BOOST_CHECK_EQUAL(f.backend->readNode(f.root).value()->metadata.revision.value(), 2);
}

BOOST_AUTO_TEST_CASE(worker_barrier_does_not_skip_http_cleanup) {
    Fixture f; f.start();
    Client partial(f.server.listenAddress());
    partial.send("POST /api/v1/nodes HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " + f.token +
                 "\r\nContent-Type: application/json\r\nContent-Length: 100\r\nExpect: 100-continue\r\n\r\n");
    BOOST_REQUIRE(drive(f.loop, [&] { partial.read(); return partial.bytes.find("100 Continue") != std::string::npos; }));
    f.stop(0.2);
    BOOST_REQUIRE(drive(f.loop, [&] { return f.runtime.metadataSnapshot().state == MetadataExecutorState::Stopped; }));
    BOOST_CHECK(!f.httpStopped);
    BOOST_CHECK(f.runtime.state() == RuntimeState::Stopping);
    f.finish(); f.durable(0);
    BOOST_CHECK(f.httpStopped);
    BOOST_CHECK_EQUAL(f.control.begins.load(), 0);
}
}  // namespace
}  // namespace anof::testing
