#include <tests/support/EventLoopWait.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/runtime/internal/ListenerFailure.h>
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

bool drive(muduox::net::EventLoop& loop, const std::function<bool()>& done,
           const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
    return anof::test::waitFor(loop, done, 3, file, line);
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
        return makeInstanceMetadataResources(root / ".anof" / "metadata.sqlite3");
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

BOOST_AUTO_TEST_CASE(listener_errors_have_stable_application_categories) {
    using internal::listenerFailure;
    BOOST_CHECK(listenerFailure({}) == ErrorCode::InternalError);
    for (const auto stage : { ListenerStage::Socket, ListenerStage::SocketSetup, ListenerStage::ReserveDescriptor,
                             ListenerStage::Bind, ListenerStage::Listen, ListenerStage::LocalAddress }) {
        const auto failure = [&](std::errc code) {
            return listenerFailure({ HttpServer::StartStatus::ListenFailed, { stage, std::make_error_code(code) } });
        };
        BOOST_CHECK(failure(std::errc::permission_denied) == ErrorCode::Forbidden);
        BOOST_CHECK(failure(std::errc::too_many_files_open) == ErrorCode::Retryable);
        BOOST_CHECK(failure(std::errc::not_enough_memory) == ErrorCode::Retryable);
        BOOST_CHECK(failure(std::errc::invalid_argument) == ErrorCode::InternalError);
        BOOST_CHECK(failure(std::errc::address_in_use) ==
                    (stage == ListenerStage::Bind ? ErrorCode::Conflict : ErrorCode::InternalError));
    }
}

BOOST_AUTO_TEST_CASE(bind_failure_joins_both_workers_before_stop_and_keeps_real_lock_until_destruction) {
    std::atomic<bool> metadataExited{false}, passwordExited{false};
    Fixture fixture;
    fixture.initialize();
    Socket listener;
    InetAddress address(0, true);
    BOOST_REQUIRE_EQUAL(::bind(listener.fd, address.getSockAddr(), sizeof(sockaddr_in)), 0);
    BOOST_REQUIRE_EQUAL(::listen(listener.fd, 1), 0);
    sockaddr_in bound{};
    socklen_t length = sizeof bound;
    BOOST_REQUIRE_EQUAL(::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&bound), &length), 0);
    auto locks = infrastructure::system::makeLinuxInstanceLockProvider();
    BOOST_REQUIRE(locks);
    auto lease = locks.value()->acquire(fixture.root);
    BOOST_REQUIRE(lease);
    InstanceConfiguration configuration;
    configuration.developmentHttp = true;
    configuration.port = ntohs(bound.sin_port);
    auto tokens = infrastructure::crypto::makeSodiumSessionTokenCodec();
    auto requests = infrastructure::crypto::makeSodiumRequestIdentity();
    BOOST_REQUIRE(tokens && requests);
    struct ExitFlag {
        std::atomic<bool>* flag;
        ~ExitFlag() { *flag = true; }
    };
    fixture.server = std::make_unique<InstanceServer>(fixture.loop,
        PreparedInstance{std::move(lease).value(), configuration},
        InstanceCrypto{std::move(tokens).value(), std::move(requests).value()}, [&] {
            thread_local ExitFlag exited{&metadataExited};
            return fixture.resources();
        }, [&] {
            thread_local ExitFlag exited{&passwordExited};
            return makeInstancePasswordResources();
        });
    const auto locked = [&] {
        const auto second = locks.value()->acquire(fixture.root);
        BOOST_REQUIRE(!second);
        BOOST_CHECK(second.error().code == ErrorCode::Conflict);
    };
    BOOST_REQUIRE(fixture.server->start([&](Result<void> ready) {
        ++fixture.readyCalls;
        BOOST_REQUIRE(!ready);
        BOOST_CHECK(ready.error().code == ErrorCode::Conflict);
        BOOST_CHECK(!fixture.server->port());
        locked();
    }, [&] {
        ++fixture.stoppedCalls;
        BOOST_CHECK(metadataExited.load());
        BOOST_CHECK(passwordExited.load());
        locked();
    }));
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.stoppedCalls == 1; }));
    BOOST_CHECK_EQUAL(fixture.readyCalls, 1U);
    BOOST_CHECK(fixture.server->state() == RuntimeState::Stopped);
    BOOST_CHECK(!fixture.server->ready());
    BOOST_CHECK_EQUAL(fixture.server->metadataSnapshot().outstanding, 0U);
    fixture.server->requestStop();
    BOOST_CHECK(!fixture.server->start());
    BOOST_CHECK_EQUAL(fixture.stoppedCalls, 1U);
    locked();
    fixture.server.reset();
    BOOST_CHECK(locks.value()->acquire(fixture.root));
}

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

BOOST_AUTO_TEST_CASE(http_cleanup_precedes_upload_dispatcher_stop) {
    Fixture fixture;
    fixture.initialize();
    bool observed = false, accepted = false, completed = false;
    const RequestContext context{{SessionDigest(SessionDigest::Bytes{}), CredentialMode::Bearer},
        RequestId::parse("10000000-0000-0000-0000-000000000001").value()};
    auto prepared = fixture.prepared();
    prepared.configuration.shutdownGraceSeconds = 1;
    http::HttpObservation observation;
    observation.committed = [&](auto) {
        if (observed) return;
        observed = true;
        fixture.server->requestStop();
        BOOST_CHECK(!fixture.server->ready());
        // HTTP 交换仍在回调栈中；上传端口必须保留至 HTTP 清理，供既有流完成。
        auto submission = fixture.server->transferDispatcher().submit(context, GetStorageCommand{},
            [&](TaskCompletion<StorageView>) { completed = true; });
        accepted = std::holds_alternative<AcceptedTask>(submission);
    };
    auto server = makeInstanceServer(fixture.loop, std::move(prepared), observation);
    BOOST_REQUIRE(server);
    fixture.server = std::move(server).value();
    fixture.start();
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.readyCalls == 1; }));
    BOOST_REQUIRE(fixture.server->port());
    Socket socket;
    InetAddress address(*fixture.server->port(), true);
    BOOST_REQUIRE_EQUAL(::connect(socket.fd, address.getSockAddr(), sizeof(sockaddr_in)), 0);
    const std::string request = "GET /health/live HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    BOOST_REQUIRE_EQUAL(::send(socket.fd, request.data(), request.size(), MSG_NOSIGNAL), static_cast<ssize_t>(request.size()));
    BOOST_REQUIRE(drive(fixture.loop, [&] { return observed; }));
    BOOST_CHECK(accepted);
    ::shutdown(socket.fd, SHUT_RDWR);
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.stoppedCalls == 1; }));
    BOOST_CHECK(completed);
    auto rejected = fixture.server->transferDispatcher().submit(context, GetStorageCommand{}, [](auto) {});
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(rejected));
    BOOST_CHECK(std::get<SubmitError>(rejected) == SubmitError::Stopping);
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
BOOST_AUTO_TEST_CASE(content_recovery_failure_never_listens_and_drains_before_lease_release) {
    Fixture fixture;
    fixture.initialize();
    const auto unknown = fixture.root / ".anof/uploads/unknown";
    { std::ofstream file(unknown); file << "preserve"; }
    fixture.production();
    fixture.start();
    BOOST_REQUIRE(drive(fixture.loop, [&] { return fixture.stoppedCalls == 1; }));
    BOOST_CHECK(fixture.startupError == ErrorCode::IntegrityError);
    BOOST_CHECK(!fixture.server->port());
    BOOST_CHECK(fixture.held);
    BOOST_CHECK_EQUAL(std::filesystem::file_size(unknown), 8);
    fixture.stopped();
}

BOOST_AUTO_TEST_CASE(active_drive_task_retires_before_instance_unlock_without_stop_cycle) {
    std::atomic<bool> armed{false};
    Fixture fixture;
    fixture.initialize();
    struct GatedClock : Clock {
        std::unique_ptr<Clock> inner;
        Fixture& fixture;
        std::atomic<bool>& armed;
        GatedClock(std::unique_ptr<Clock> c, Fixture& f, std::atomic<bool>& a)
            : inner(std::move(c)), fixture(f), armed(a) {}
        UtcTime now() const override {
            if (armed.exchange(false)) {
                std::unique_lock<std::mutex> lock(fixture.mutex);
                fixture.entered = true;
                if (!fixture.changed.wait_for(lock, std::chrono::seconds(5), [&]{return fixture.released;}))
                    throw std::runtime_error("drive gate timeout");
            }
            return inner->now();
        }
    };
    fixture.injected([&]() -> Result<MetadataWorkerResources> {
        auto resources = fixture.resources();
        if (!resources) return resources.error();
        resources.value().clock = std::make_unique<GatedClock>(std::move(resources.value().clock), fixture, armed);
        return std::move(resources).value();
    });
    fixture.start();
    BOOST_REQUIRE(drive(fixture.loop, [&]{return fixture.server->ready();}));
    // ready可在启动完成项的回调内发布；容量为1时须等该任务退休后再设置业务闸门。
    BOOST_REQUIRE(drive(fixture.loop, [&]{return fixture.server->metadataSnapshot().outstanding == 0;}));
    const RequestContext context{{SessionDigest(SessionDigest::Bytes{}), CredentialMode::Bearer},
        RequestId::parse("10000000-0000-0000-0000-000000000001").value()};
    unsigned completed = 0;
    armed = true;
    auto task = fixture.server->driveDispatcher().submit(context,
        GetTrashCommand{TrashId::parse("10000000-0000-0000-0000-000000000001").value()},
        [&](TaskCompletion<TrashView> c) {
            BOOST_REQUIRE(std::holds_alternative<Result<TrashView>>(c.outcome));
            auto& result = std::get<Result<TrashView>>(c.outcome);
            BOOST_REQUIRE(!result); BOOST_CHECK(result.error().code == ErrorCode::Unauthenticated);
            BOOST_CHECK(fixture.held); ++completed;
        });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(task));
    BOOST_REQUIRE(drive(fixture.loop, [&]{return fixture.entered.load();}));
    fixture.server->requestStop();
    bool stopObserved = false;
    fixture.loop.queueInLoop([&]{stopObserved = true;});
    BOOST_REQUIRE(drive(fixture.loop, [&]{return stopObserved;}));
    // 上传清理也可能等待metadata；不要求worker在运行中Drive退休前进入Stopping。
    BOOST_CHECK(fixture.server->state() == RuntimeState::Stopping);
    BOOST_CHECK(fixture.held); BOOST_CHECK_EQUAL(fixture.stoppedCalls, 0); BOOST_CHECK_EQUAL(completed, 0);
    fixture.release();
    fixture.stopped();
    BOOST_CHECK_EQUAL(completed, 1);
}

}  // namespace
}  // namespace anof::testing
