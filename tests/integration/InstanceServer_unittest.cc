#include <anof/runtime/InstanceIdentity.h>
#define BOOST_TEST_MODULE InstanceServerIntegration
#include <boost/test/unit_test.hpp>

#include <anof/runtime/InstanceServer.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <sys/stat.h>
#include <fstream>
#include <anof/infrastructure/crypto/SodiumNodeIdGenerator.h>
#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <tests/integration/InitializedInstance.h>
#include <anof/infrastructure/system/SystemClock.h>
#include <muduox/net/EventLoop.h>
#include <muduox/net/InetAddress.h>

#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace anof::testing {
namespace {
using namespace anof::runtime;
using namespace muduox::net;

bool drive(EventLoop& loop, const std::function<bool()>& predicate) {
    if (predicate()) return true;
    bool expired = false;
    const auto poll = loop.runEvery(0.001, [&] { if (predicate()) loop.quit(); });
    const auto deadline = loop.runAfter(3, [&] { expired = true; loop.quit(); });
    loop.loop();
    loop.cancel(poll);
    loop.cancel(deadline);
    return !expired && predicate();
}

// 只观测所有权，不声称验证真实 flock；跨进程锁由 infrastructure 适配测试负责。
class Lease final : public InstanceLease {
public:
    Lease(std::filesystem::path root, bool& held) : root_(std::move(root)), held_(held) { held_ = true; }
    ~Lease() override { held_ = false; }
    const std::filesystem::path& root() const noexcept override { return root_; }
private:
    std::filesystem::path root_;
    bool& held_;
};

struct Fixture {
    EventLoop loop;
    std::filesystem::path root;
    bool held = false;
    std::unique_ptr<InstanceServer> server;
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;
    std::atomic<bool> entered{false};
    unsigned readyCalls = 0, stoppedCalls = 0;
    std::optional<ErrorCode> startupError;
    Fixture() {
        char name[] = "/tmp/anof-startup-XXXXXX";
        auto directory = ::mkdtemp(name);
        if (!directory) throw std::runtime_error("temporary root failed");
        root = directory;
    }
    ~Fixture() {
        release();
        if (server) {
            server->requestStop();
            if (!drive(loop, [&] { return server->state() == RuntimeState::Stopped; })) std::terminate();
            server.reset();
        }
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    PreparedInstance prepared() {
        InstanceConfiguration configuration;
        configuration.developmentHttp = true;
        configuration.port = 0;
        configuration.metadataQueue = 1;
        configuration.shutdownGraceSeconds = 0;
        return { std::make_unique<Lease>(root, held), configuration };
    }
    void initialize() {
        BOOST_REQUIRE(initializeTestInstance(root));
    }
    Result<MetadataWorkerResources> resources() {
        auto store = infrastructure::sqlite::openSqliteMetadataStore(root / ".anof" / "metadata.sqlite3", {instancePasswordPolicy().verification, {}});
        if (!store) return store.error();
        auto clock = infrastructure::system::makeSystemClock();
        if (!clock) return clock.error();
        auto ids = infrastructure::crypto::makeSodiumNodeIdGenerator();
        if (!ids) return ids.error();
        return MetadataWorkerResources{ std::move(store).value(), std::move(clock).value(), std::move(ids).value() };
    }
    void production() {
        auto result = makeInstanceServer(loop, prepared());
        BOOST_REQUIRE(result);
        server = std::move(result).value();
    }
    void injected(MetadataExecutor::Factory factory, IdentityExecutor::Factory password = makeInstancePasswordResources) {
        auto tokens = infrastructure::crypto::makeSodiumSessionTokenCodec();
        auto requests = infrastructure::crypto::makeSodiumRequestIdentity();
        BOOST_REQUIRE(tokens);
        BOOST_REQUIRE(requests);
        server = std::make_unique<InstanceServer>(loop, prepared(),
            InstanceCrypto{ std::move(tokens).value(), std::move(requests).value() }, std::move(factory), std::move(password));
    }
    void start(bool throwReady = false, bool throwStopped = false) {
        BOOST_REQUIRE(server->start([this, throwReady](Result<void> result) {
            ++readyCalls;
            if (!result) startupError = result.error().code;
            BOOST_CHECK(held);
            if (throwReady) throw std::runtime_error("ready observer failed");
        }, [this, throwStopped] {
            ++stoppedCalls;
            BOOST_CHECK(held);
            if (throwStopped) throw std::runtime_error("stopped observer failed");
        }));
    }
    void release() {
        { std::lock_guard<std::mutex> lock(mutex); released = true; }
        changed.notify_all();
    }
    Result<MetadataWorkerResources> gated() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return released; })) return ErrorCode::InternalError;
        lock.unlock();
        return resources();
    }
    void stopped() {
        BOOST_REQUIRE(drive(loop, [&] { return server->state() == RuntimeState::Stopped; }));
        BOOST_CHECK(held);
        BOOST_CHECK_EQUAL(server->metadataSnapshot().outstanding, 0U);
        BOOST_CHECK_EQUAL(readyCalls, 1U);
        BOOST_CHECK_EQUAL(stoppedCalls, 1U);
        BOOST_CHECK(!server->ready());
        server.reset();
        BOOST_CHECK(!held);
    }
};

struct Socket {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    Socket() { if (fd < 0) throw std::runtime_error("socket failed"); }
    ~Socket() { ::close(fd); }
};

BOOST_AUTO_TEST_CASE(production_factory_delays_http_until_worker_ready_and_keeps_lease_through_stop) {
    Fixture fixture;
    fixture.initialize();
    fixture.production();
    BOOST_CHECK(!fixture.server->port());
    BOOST_CHECK(!fixture.server->ready());
    fixture.start();
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.readyCalls == 1; }));
    BOOST_CHECK(!fixture.startupError);
    BOOST_REQUIRE(fixture.server->ready());
    BOOST_REQUIRE(fixture.server->port());
    Socket socket;
    InetAddress address(*fixture.server->port(), true);
    BOOST_REQUIRE_EQUAL(::connect(socket.fd, address.getSockAddr(), sizeof(sockaddr_in)), 0);
    const std::string request = "GET /health/ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    BOOST_REQUIRE_EQUAL(::send(socket.fd, request.data(), request.size(), MSG_NOSIGNAL | MSG_DONTWAIT), static_cast<ssize_t>(request.size()));
    std::string response;
    BOOST_REQUIRE(drive(fixture.loop, [&] {
        std::array<char, 4096> buffer{};
        const auto n = ::recv(socket.fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (n > 0) response.append(buffer.data(), static_cast<std::size_t>(n));
        if (response.size() > 8192) throw std::runtime_error("response limit");
        return n == 0;
    }));
    BOOST_CHECK(response.find("HTTP/1.1 200 OK\r\n") == 0);
    BOOST_CHECK(response.find("{\"status\":\"ready\"}") != std::string::npos);
    fixture.server->requestStop();
    BOOST_CHECK(!fixture.server->ready());
    fixture.server->requestStop();
    BOOST_CHECK(!fixture.server->start());
    fixture.stopped();
}

BOOST_AUTO_TEST_CASE(password_startup_failure_never_binds_and_keeps_lease_until_destruction) {
    Fixture fixture;
    fixture.initialize();
    fixture.injected([&] { return fixture.resources(); }, []() -> Result<PasswordWorkerResources> { return ErrorCode::InternalError; });
    fixture.start();
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.server->state() == RuntimeState::Stopped; }));
    BOOST_CHECK(fixture.startupError == ErrorCode::InternalError);
    BOOST_CHECK(!fixture.server->port());
    fixture.stopped();
}

BOOST_AUTO_TEST_CASE(missing_database_fails_before_binding_without_creating_database) {
    Fixture fixture;
    fixture.production();
    fixture.start();
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.stoppedCalls == 1; }));
    BOOST_CHECK(fixture.startupError.has_value());
    BOOST_CHECK(!fixture.server->port());
    BOOST_CHECK(!std::filesystem::exists(fixture.root / ".anof" / "metadata.sqlite3"));
    fixture.stopped();
}

BOOST_AUTO_TEST_CASE(stop_during_worker_initialization_never_publishes_ready_or_releases_lease_early) {
    Fixture fixture;
    fixture.initialize();
    fixture.injected([&] { return fixture.gated(); });
    fixture.start();
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.entered.load(); }));
    fixture.server->requestStop();
    BOOST_CHECK(fixture.server->state() == RuntimeState::Stopping);
    BOOST_CHECK(fixture.held);
    BOOST_CHECK(!fixture.server->port());
    BOOST_CHECK_EQUAL(fixture.stoppedCalls, 0U);
    fixture.release();
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.stoppedCalls == 1; }));
    BOOST_CHECK(fixture.startupError == ErrorCode::Retryable);
    BOOST_CHECK(!fixture.server->port());
    fixture.stopped();
}

BOOST_AUTO_TEST_CASE(worker_factory_exception_is_a_failed_start_without_http) {
    Fixture fixture;
    fixture.injected([]() -> Result<MetadataWorkerResources> { throw std::runtime_error("worker initialization failed"); });
    fixture.start();
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.stoppedCalls == 1; }));
    BOOST_CHECK(fixture.startupError == ErrorCode::InternalError);
    BOOST_CHECK(!fixture.server->port());
    fixture.stopped();
}

BOOST_AUTO_TEST_CASE(ready_observer_exception_stops_and_stopped_observer_exception_is_contained) {
    Fixture fixture;
    fixture.initialize();
    fixture.production();
    fixture.start(true, true);
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.server->state() == RuntimeState::Stopped; }));
    BOOST_CHECK_EQUAL(fixture.server->notificationExceptions(), 2U);
    fixture.stopped();
}

BOOST_AUTO_TEST_CASE(invalid_prepared_configuration_releases_lease_without_worker_or_http) {
    Fixture fixture;
    auto prepared = fixture.prepared();
    prepared.configuration.developmentHttp = false;
    auto result = makeInstanceServer(fixture.loop, std::move(prepared));
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == ErrorCode::InvalidArgument);
    BOOST_CHECK(!fixture.held);
}

BOOST_AUTO_TEST_CASE(real_lease_survives_cancelled_worker_start_until_instance_destruction) {
    Fixture fixture;
    fixture.initialize();
    auto locks = infrastructure::system::makeLinuxInstanceLockProvider();
    auto config = infrastructure::config::makeTomlInstanceConfigurationLoader();
    auto maintenance = infrastructure::sqlite::makeSqliteInstanceMaintenance({instancePasswordPolicy().verification, {}});
    BOOST_REQUIRE(locks && config && maintenance);
    auto prepared = prepareInstance(fixture.root, *locks.value(), *config.value(), *maintenance.value());
    BOOST_REQUIRE(prepared);
    auto tokens = infrastructure::crypto::makeSodiumSessionTokenCodec();
    auto requests = infrastructure::crypto::makeSodiumRequestIdentity();
    BOOST_REQUIRE(tokens && requests);
    fixture.server = std::make_unique<InstanceServer>(fixture.loop, std::move(prepared).value(),
        InstanceCrypto{std::move(tokens).value(), std::move(requests).value()}, [&] { return fixture.gated(); }, makeInstancePasswordResources);
    const auto locked = [&] {
        const auto second = locks.value()->acquire(fixture.root);
        BOOST_REQUIRE(!second);
        BOOST_CHECK(second.error().code == ErrorCode::Conflict);
    };
    BOOST_REQUIRE(fixture.server->start([&](Result<void> ready) {
        BOOST_REQUIRE(!ready);
        BOOST_CHECK(ready.error().code == ErrorCode::Retryable);
        ++fixture.readyCalls;
    }));
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.entered.load(); }));
    locked();
    fixture.server->requestStop();
    BOOST_CHECK(fixture.server->state() == RuntimeState::Stopping);
    locked();
    fixture.release();
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.server->state() == RuntimeState::Stopped; }));
    BOOST_CHECK_EQUAL(fixture.readyCalls, 1U);
    BOOST_CHECK(!fixture.server->port());
    locked();
    fixture.server.reset();
    BOOST_CHECK(locks.value()->acquire(fixture.root));
}

BOOST_AUTO_TEST_CASE(stop_before_start_is_terminal) {
    Fixture fixture;
    fixture.production();
    fixture.server->requestStop();
    BOOST_CHECK(fixture.server->state() == RuntimeState::Stopped);
    BOOST_CHECK(!fixture.server->start());
    BOOST_CHECK(!fixture.server->port());
    fixture.server.reset();
    BOOST_CHECK(!fixture.held);
}
}  // namespace
}  // namespace anof::testing
