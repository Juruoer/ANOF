#include <anof/runtime/InstanceServer.h>

#include <anof/infrastructure/crypto/SodiumNodeIdGenerator.h>
#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/system/SystemClock.h>
#include <muduox/net/EventLoop.h>
#include <muduox/net/InetAddress.h>

#include <stdexcept>
#include <utility>

namespace anof::runtime {

InstanceServer::InstanceServer(muduox::net::EventLoop& loop, PreparedInstance prepared, InstanceCrypto crypto,
                               MetadataExecutor::Factory worker, http::HttpObservation httpObservation,
                               http::DirectoryTaskObserver taskObservation)
    : loop_(loop), prepared_(std::move(prepared)), crypto_(std::move(crypto)), worker_(std::move(worker)),
      httpObservation_(std::move(httpObservation)), taskObservation_(std::move(taskObservation)),
      runtime_(loop, { prepared_.configuration.metadataQueue, 32 }) {
    loop_.assertInLoopThread();
    if (!prepared_.lease || !prepared_.lease->root().is_absolute() || !crypto_.tokens || !crypto_.requests || !worker_ ||
        !validateInstanceConfiguration(prepared_.configuration)) throw std::invalid_argument("Invalid instance dependencies");
}

InstanceServer::~InstanceServer() {
    loop_.assertInLoopThread();
    if (startCalled_ && !completed_) std::terminate();
    // server／api、runtime、crypto 均先于 prepared_ 销毁；锁覆盖完整清理过程。
}

Result<void> InstanceServer::start(MetadataExecutor::Started readyCallback, MetadataExecutor::Stopped stopped) {
    loop_.assertInLoopThread();
    if (startCalled_ || stopCalled_) return ErrorCode::InvalidArgument;
    startCalled_ = true;
    onReady_ = std::move(readyCallback);
    onStopped_ = std::move(stopped);
    auto result = runtime_.start(std::move(worker_), [this](auto initializedResult) { initialized(initializedResult); },
                                 [this] { finishIfStopped(); });
    if (!result) {
        completed_ = true;
        onReady_ = {};
        onStopped_ = {};
    }
    return result;
}

void InstanceServer::initialized(Result<void> result) {
    if (!result) {
        notifyReady(result);
        return;
    }
    if (stopCalled_) {
        notifyReady(ErrorCode::Retryable);
        return;
    }
    try {
        http::CreateDirectoryHandler handler(runtime_.dispatcher(), [this] { return ready(); }, taskObservation_);
        api_ = std::make_unique<http::HttpApi>(*crypto_.tokens, *crypto_.requests, [this] { return ready(); },
            handler.stream(), httpObservation_, http::HttpApiOptions{ prepared_.configuration.httpInFlight, 15, 30 });
        // muduox 构造即 bind；必须延后到 worker 初始化成功。bind/listen 的 fatal 不可捕获。
        server_ = std::make_unique<muduox::net::HttpServer>(&loop_,
            muduox::net::InetAddress(prepared_.configuration.port, true), "anof");
        api_->configure(*server_);
        server_->start();
        httpStarted_ = true;
        httpStopped_ = false;
        port_ = server_->listenAddress().port();
        if (!runtime_.markHttpStarted()) throw std::logic_error("Unexpected instance startup state");
    } catch (...) {
        // 即使 HTTP start 抛出，仍先执行真实 stop；外层屏障不会只等 metadata。
        if (server_) httpStopped_ = false;
        requestStop();
        notifyReady(ErrorCode::InternalError);
        return;
    }
    notifyReady({});
}

void InstanceServer::notifyReady(Result<void> result) {
    auto callback = std::move(onReady_);
    try { if (callback) callback(result); }
    catch (...) { ++notificationExceptions_; requestStop(); }
}

void InstanceServer::requestStop() {
    loop_.assertInLoopThread();
    if (stopCalled_) return;
    stopCalled_ = true;
    runtime_.requestStop();
    if (server_ && !httpStopped_) {
        server_->stop(prepared_.configuration.shutdownGraceSeconds, [this] {
            httpStopped_ = true;
            if (httpStarted_) (void)runtime_.markHttpStopped();
            finishIfStopped();
        });
    }
    finishIfStopped();
}

void InstanceServer::finishIfStopped() {
    if (completed_ || runtime_.state() != RuntimeState::Stopped || !httpStopped_) return;
    completed_ = true;
    auto callback = std::move(onStopped_);
    try { if (callback) callback(); }
    catch (...) { ++notificationExceptions_; }
}

RuntimeState InstanceServer::state() const {
    loop_.assertInLoopThread();
    if (completed_) return RuntimeState::Stopped;
    if (stopCalled_ || runtime_.state() == RuntimeState::Stopped) return RuntimeState::Stopping;
    return runtime_.state();
}

bool InstanceServer::ready() const { return state() == RuntimeState::Ready; }
std::optional<std::uint16_t> InstanceServer::port() const { loop_.assertInLoopThread(); return port_; }
MetadataExecutorSnapshot InstanceServer::metadataSnapshot() const { return runtime_.metadataSnapshot(); }
std::size_t InstanceServer::notificationExceptions() const { loop_.assertInLoopThread(); return notificationExceptions_; }

Result<std::unique_ptr<InstanceServer>> makeInstanceServer(muduox::net::EventLoop& loop, PreparedInstance prepared,
    http::HttpObservation httpObservation, http::DirectoryTaskObserver taskObservation) {
    try {
        if (!prepared.lease || !prepared.lease->root().is_absolute()) return ErrorCode::InvalidArgument;
        auto valid = validateInstanceConfiguration(prepared.configuration);
        if (!valid) return valid.error();
        auto tokens = infrastructure::crypto::makeSodiumSessionTokenCodec();
        if (!tokens) return tokens.error();
        auto requests = infrastructure::crypto::makeSodiumRequestIdentity();
        if (!requests) return requests.error();
        const auto database = prepared.lease->root() / ".anof" / "metadata.sqlite3";
        auto worker = [database]() -> Result<MetadataWorkerResources> {
            auto store = infrastructure::sqlite::openSqliteMetadataStore(database);
            if (!store) return store.error();
            auto clock = infrastructure::system::makeSystemClock();
            if (!clock) return clock.error();
            auto ids = infrastructure::crypto::makeSodiumNodeIdGenerator();
            if (!ids) return ids.error();
            return MetadataWorkerResources{ std::move(store).value(), std::move(clock).value(), std::move(ids).value() };
        };
        return std::make_unique<InstanceServer>(loop, std::move(prepared),
            InstanceCrypto{ std::move(tokens).value(), std::move(requests).value() }, std::move(worker),
            std::move(httpObservation), std::move(taskObservation));
    } catch (...) { return ErrorCode::InternalError; }
}

}  // namespace anof::runtime
