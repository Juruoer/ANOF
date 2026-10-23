#pragma once
#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/application/identity/IdentityService.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/runtime/InstanceServer.h>
#include <muduox/net/EventLoop.h>
#include <muduox/net/InetAddress.h>
#include <sys/socket.h>
#include <tests/integration/InitializedInstance.h>
#include <tests/integration/TransferFaultStore.h>
#include <tests/support/EventLoopWait.h>
#include <unistd.h>

#include <atomic>
#include <boost/test/unit_test.hpp>
#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
using namespace anof;
using namespace anof::runtime;
using namespace anof::infrastructure;
using Json = nlohmann::json;
namespace {
bool drive(muduox::net::EventLoop& loop, const std::function<bool()>& done,
           const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
    return anof::test::waitFor(loop, done, 5, file, line);
}
struct Client {
    int fd;
    std::string bytes;
    bool eof = false;
    std::size_t limit = 512 * 1024;
    explicit Client(std::uint16_t port) : fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        if (fd < 0) throw std::runtime_error("socket failed");
        muduox::net::InetAddress address(port, true);
        if (::connect(fd, address.getSockAddr(), sizeof(sockaddr_in)) != 0) {
            ::close(fd);
            throw std::runtime_error("connect failed");
        }
    }
    ~Client() { ::close(fd); }
    void send(const std::string& text) {
        BOOST_REQUIRE_EQUAL(::send(fd, text.data(), text.size(), MSG_NOSIGNAL | MSG_DONTWAIT), static_cast<ssize_t>(text.size()));
    }
    void read() {
        char buffer[4096];
        for (;;) {
            auto n = ::recv(fd, buffer, sizeof buffer, MSG_DONTWAIT);
            if (n > 0) {
                bytes.append(buffer, static_cast<std::size_t>(n));
                if (bytes.size() > limit) throw std::runtime_error("response limit");
            } else if (n == 0 || (n < 0 && errno == ECONNRESET)) {
                eof = true;
                return;
            } else if (errno == EINTR)
                continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            else
                throw std::runtime_error("receive failed");
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
Json loginBody(std::string name = "administrator") { return { { "login", name }, { "password", "fixture password only" }, { "credential_mode", "bearer" } }; }
Json registration(std::string name, std::string token) { return { { "login", name }, { "display_name", name }, { "password", "fixture password only" }, { "invite_token", token } }; }
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> entered{ false };
    bool released = false;
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        cv.notify_all();
    }
};
struct Hasher final : PasswordHasher {
    std::unique_ptr<PasswordHasher> inner;
    Gate& gate;
    Hasher(std::unique_ptr<PasswordHasher> value, Gate& g) : inner(std::move(value)), gate(g) {}
    Result<EncodedPasswordHash> hash(const PasswordInput& input) override { return inner->hash(input); }
    Result<bool> verify(const PasswordInput& input, const EncodedPasswordHash& encoded) override {
        gate.entered = true;
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (!gate.cv.wait_for(lock, std::chrono::seconds(4), [&] { return gate.released; })) return ErrorCode::InternalError;
        lock.unlock();
        return inner->verify(input, encoded);
    }
};
// 只对发生身份或文件管理审计写入的事务注入“已提交但确认丢失”，清理事务保持真实结果。
struct AuditProbe final : IdentityAuditRepository {
    IdentityAuditRepository& inner;
    bool wrote = false;
    explicit AuditProbe(IdentityAuditRepository& value) : inner(value) {}
    Result<void> append(const IdentityEvent& event) override {
        wrote = true;
        return inner.append(event);
    }
};
struct DriveAuditProbe final : DriveAuditRepository {
    DriveAuditRepository& inner;
    bool wrote = false;
    explicit DriveAuditProbe(DriveAuditRepository& value) : inner(value) {}
    Result<void> append(const DriveEvent& event) override {
        wrote = true;
        return inner.append(event);
    }
};
struct UncertainTransaction final : DriveTransaction {
    std::unique_ptr<MetadataTransaction> inner;
    AuditProbe audit;
    DriveAuditProbe driveAuditProbe;
    explicit UncertainTransaction(std::unique_ptr<MetadataTransaction> value) : inner(std::move(value)), audit(inner->identityAudit()),
        driveAuditProbe(dynamic_cast<DriveTransaction&>(*inner).driveAudit()) {}
    NodeAccountingRepository& nodeAccounting() override { return inner->nodeAccounting(); }
    TreeRepository& tree() override { return dynamic_cast<DriveTransaction&>(*inner).tree(); }
    TrashRepository& trash() override { return dynamic_cast<DriveTransaction&>(*inner).trash(); }
    DriveAuditRepository& driveAudit() override { return driveAuditProbe; }
    AuthorizationReader& authorization() override { return inner->authorization(); }
    UserRepository& users() override { return inner->users(); }
    SessionRepository& sessions() override { return inner->sessions(); }
    InviteRepository& invites() override { return inner->invites(); }
    SpaceRepository& spaces() override { return inner->spaces(); }
    FileNodeRepository& nodes() override { return dynamic_cast<TransferTransaction&>(*inner).nodes(); }
    DriveQuotaRepository& quotas() override { return dynamic_cast<DriveTransaction&>(*inner).quotas(); }
    DriveUploadRepository& uploads() override { return dynamic_cast<DriveTransaction&>(*inner).uploads(); }
    LifecycleBlobRepository& blobs() override { return dynamic_cast<DriveTransaction&>(*inner).blobs(); }
    DriveStorageRepository& storage() override { return dynamic_cast<DriveTransaction&>(*inner).storage(); }
    TransferAuditRepository& transferAudit() override { return dynamic_cast<TransferTransaction&>(*inner).transferAudit(); }
    SettingsRepository& settings() override { return inner->settings(); }
    AuditRepository& directoryAudit() override { return inner->directoryAudit(); }
    IdentityAuditRepository& identityAudit() override { return audit; }
    Result<void> commit() override {
        auto result = inner->commit();
        return result && (audit.wrote || driveAuditProbe.wrote) ? Result<void>{ ErrorCode::CommitOutcomeUnknown } : result;
    }
};
struct UncertainStore final : DriveMetadataStore {
    std::unique_ptr<MetadataStore> inner;
    explicit UncertainStore(std::unique_ptr<MetadataStore> value) : inner(std::move(value)) {}
    Result<std::unique_ptr<DriveReadSnapshot>> beginDriveRead() override { return dynamic_cast<DriveMetadataStore&>(*inner).beginDriveRead(); }
    Result<std::unique_ptr<DriveTransaction>> beginDriveWrite() override {
        auto tx = dynamic_cast<DriveMetadataStore&>(*inner).beginDriveWrite(); if (!tx) return tx.error();
        return std::unique_ptr<DriveTransaction>(new UncertainTransaction(std::move(tx).value()));
    }
    Result<std::unique_ptr<TransferReadSnapshot>> beginTransferRead() override {
        return dynamic_cast<TransferMetadataStore&>(*inner).beginTransferRead();
    }
    Result<std::unique_ptr<TransferTransaction>> beginTransferWrite() override {
        auto tx = dynamic_cast<TransferMetadataStore&>(*inner).beginTransferWrite();
        if (!tx) return tx.error();
        return std::unique_ptr<TransferTransaction>(new UncertainTransaction(std::move(tx).value()));
    }
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override { return inner->beginRead(); }
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override {
        auto result = inner->beginWrite();
        if (!result) return result.error();
        return std::unique_ptr<MetadataTransaction>(new UncertainTransaction(std::move(result).value()));
    }
};
// 测试线程设置读数，metadata worker 读取；不调整系统时钟。
struct ControlledClock final : Clock {
    std::shared_ptr<std::atomic<std::int64_t>> micros;
    explicit ControlledClock(std::shared_ptr<std::atomic<std::int64_t>> value) : micros(std::move(value)) {}
    UtcTime now() const override { return UtcTime{std::chrono::microseconds{micros->load()}}; }
};
struct Fixture {
    std::filesystem::path root;
    muduox::net::EventLoop loop;
    Gate gate;
    std::shared_ptr<std::atomic<std::int64_t>> controlledUtc;
    anof::test::CommitFault transferFault;
    std::unique_ptr<InstanceServer> server;
    explicit Fixture(std::int64_t budget = 1048576) {
        char pattern[] = "/tmp/anof-http-identity-XXXXXX";
        auto* path = ::mkdtemp(pattern);
        BOOST_REQUIRE(path);
        root = path;
        auto options = anof::testing::testInitOptions(root);
        options.instance.configuration.shutdownGraceSeconds = 0;
        options.contentBudget = ByteCount::from(budget).value();
        BOOST_REQUIRE(initializeInstance(options, PasswordInput::parse("fixture password only").value()));
    }
    ~Fixture() {
        gate.release();
        stop();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void stop() {
        if (!server) return;
        server->requestStop();
        if (!drive(loop, [&] { return server->state() == RuntimeState::Stopped; })) std::terminate();
        server.reset();
    }
    void start(IdentityExecutor::Factory password = {}, bool uncertain = false, bool transferFaults = false, double grace = 0) {
        auto locks = system::makeLinuxInstanceLockProvider();
        auto config = config::makeTomlInstanceConfigurationLoader();
        auto maintenance = sqlite::makeSqliteInstanceMaintenance({ instancePasswordPolicy().verification, {} });
        BOOST_REQUIRE(locks && config && maintenance);
        auto prepared = prepareInstance(root, *locks.value(), *config.value(), *maintenance.value());
        BOOST_REQUIRE(prepared);
        prepared.value().configuration.shutdownGraceSeconds = grace;
        if (!password && !uncertain && !transferFaults && !controlledUtc) {
            auto created = makeInstanceServer(loop, std::move(prepared).value());
            BOOST_REQUIRE(created);
            server = std::move(created).value();
        } else {
            auto codec = crypto::makeSodiumSessionTokenCodec();
            auto ids = crypto::makeSodiumRequestIdentity();
            BOOST_REQUIRE(codec && ids);
            server = std::make_unique<InstanceServer>(loop, std::move(prepared).value(), InstanceCrypto{ std::move(codec).value(), std::move(ids).value() }, [this, uncertain, transferFaults]() -> Result<MetadataWorkerResources> {
                    auto resources = makeInstanceMetadataResources(root / ".anof/metadata.sqlite3");
                    if (!resources) return resources.error();
                    if (controlledUtc) resources.value().clock = std::make_unique<ControlledClock>(controlledUtc);
                    if (transferFaults) {
                        auto* store = dynamic_cast<TransferMetadataStore*>(resources.value().metadata.get());
                        if (!store) return ErrorCode::InternalError;
                        resources.value().metadata.release();
                        resources.value().metadata = std::make_unique<anof::test::TransferFaultStore>(
                            std::unique_ptr<TransferMetadataStore>(store), transferFault);
                    }
                    if (uncertain) resources.value().metadata = std::make_unique<UncertainStore>(std::move(resources.value().metadata));
                    return std::move(resources).value(); }, password ? std::move(password) : IdentityExecutor::Factory{ makeInstancePasswordResources });
        }
        bool ready = false;
        BOOST_REQUIRE(server->start([&](Result<void> result) { BOOST_REQUIRE(result); ready = true; }));
        BOOST_REQUIRE(drive(loop, [&] { return ready; }));
        BOOST_REQUIRE(server->ready());
    }
    std::string exchange(const std::string& text) {
        Client client(*server->port());
        client.send(text);
        BOOST_REQUIRE(drive(loop, [&] { client.read(); return client.eof; }));
        return client.bytes;
    }
    Json call(std::string method, std::string path, int status, const Json& body = nullptr, std::string token = {}, std::string extra = {}) {
        auto wire = exchange(request(method, path, body, token, extra));
        BOOST_REQUIRE_MESSAGE(wire.find("HTTP/1.1 " + std::to_string(status) + " ") == 0,
            method << " " << path.substr(0, path.find('?')) << " expected=" << status << " response=" << wire);
        BOOST_CHECK(wire.find("cache-control: no-store\r\n") != std::string::npos);
        BOOST_CHECK(wire.find("x-request-id: ") != std::string::npos);
        auto split = wire.find("\r\n\r\n");
        BOOST_REQUIRE(split != std::string::npos);
        auto payload = wire.substr(split + 4);
        if (status == 204) {
            BOOST_CHECK(payload.empty());
            return nullptr;
        }
        return Json::parse(payload);
    }
    std::string login(std::string name = "administrator") { return call("POST", "/api/v1/auth/sessions", 201, loginBody(name))["access_token"]; }
    void locked() {
        auto locks = system::makeLinuxInstanceLockProvider();
        BOOST_REQUIRE(locks);
        auto lock = locks.value()->acquire(root);
        BOOST_REQUIRE(!lock);
        BOOST_CHECK(lock.error().code == ErrorCode::Conflict);
    }
};
}  // namespace
