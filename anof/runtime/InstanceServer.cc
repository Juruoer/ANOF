#include <anof/runtime/internal/ShutdownTrace.h>
#include <anof/runtime/internal/ListenerFailure.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/runtime/InstanceServer.h>

#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <anof/infrastructure/storage/local/LocalTransferFileStore.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <muduox/net/EventLoop.h>
#include <muduox/net/InetAddress.h>

#include <stdexcept>
#include <utility>

namespace anof::runtime {

InstanceServer::InstanceServer(muduox::net::EventLoop& loop, PreparedInstance prepared, InstanceCrypto crypto,
                               MetadataExecutor::Factory worker, IdentityExecutor::Factory passwordWorker, http::HttpObservation httpObservation,
                               http::DirectoryTaskObserver taskObservation)
    : loop_(loop), prepared_(std::move(prepared)), crypto_(std::move(crypto)), worker_(std::move(worker)),
      passwordWorker_(std::move(passwordWorker)),
      httpObservation_(std::move(httpObservation)), taskObservation_(std::move(taskObservation)),
      runtime_(loop, { prepared_.configuration.metadataQueue, 32 }), identity_(loop, runtime_.metadataExecutor()),
      disk_(loop), uploads_(loop, runtime_.metadataExecutor(), disk_, prepared_.configuration.diskSafety) {
    loop_.assertInLoopThread();
    identity_.setUserDisabledObserver([this](UserId user, bool confirmed){uploads_.notifyUserDisabled(user, confirmed);});
    if (!prepared_.lease || !prepared_.lease->root().is_absolute() || !crypto_.tokens || !crypto_.requests || !worker_ || !passwordWorker_ ||
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
    if (!result || stopCalled_) {
        requestStop();
        notifyReady(result ? Result<void>{ErrorCode::Retryable} : result);
        return;
    }
    auto started = identity_.start(std::move(passwordWorker_), [this](auto ready) { identityInitialized(ready); },
                                   [this] { stopMetadata(); });
    if (!started) { requestStop(); notifyReady(started); }
}

void InstanceServer::identityInitialized(Result<void> result) {
    if (!result || stopCalled_) {
        requestStop();
        notifyReady(result ? Result<void>{ErrorCode::Retryable} : result);
        return;
    }
    const auto root=prepared_.lease->root();
    auto started=disk_.start([root]()->Result<std::unique_ptr<TransferFileStore>> {
        auto opened=infrastructure::storage::openLocalTransferFileStore(root);
        if(auto* store=std::get_if<std::unique_ptr<TransferFileStore>>(&opened))return std::move(*store);
        return ErrorCode::IntegrityError;
    },[this](auto ready){transferInitialized(ready);},[this]{stopMetadata();});
    if(!started){requestStop();notifyReady(started);}
}
void InstanceServer::transferInitialized(Result<void> result) {
    if(!result || stopCalled_){requestStop();notifyReady(result?Result<void>{ErrorCode::Retryable}:result);return;}
    uploads_.recover([this](auto recovered){
        if(!recovered || stopCalled_){requestStop();notifyReady(recovered?Result<void>{ErrorCode::Retryable}:recovered);return;}
        listen();
    });
}
void InstanceServer::listen() {
    try {
        http::CreateDirectoryHandler handler(runtime_.dispatcher(), [this] { return ready(); }, taskObservation_);
        api_ = std::make_unique<http::HttpApi>(*crypto_.tokens, *crypto_.requests, [this] { return ready(); },
            handler.stream(), identityDispatcher(), transferDispatcher(), httpObservation_, http::HttpApiOptions{ prepared_.configuration.httpInFlight, 15, 30 });
        server_ = std::make_unique<muduox::net::HttpServer>(muduox::net::HttpServer::DeferredListen{}, &loop_,
            muduox::net::InetAddress(prepared_.configuration.port, true), "anof");
        api_->configure(*server_);
        const auto started = server_->tryStart();
        if (!started) {
            // 监听错误已同步释放库内资源；应用 worker 仍须退出，lease 留到实例析构。
            if (started.status == muduox::net::HttpServer::StartStatus::ListenFailed) server_.reset();
            else httpStopped_ = false;
            requestStop();
            notifyReady(internal::listenerFailure(started));
            return;
        }
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
    internal::shutdownTrace("instance", "stop_requested", this);
    identity_.requestStop();
    // ready() 已关闭新 HTTP 准入；现有 PATCH 在 HTTP grace 内仍可 feed/finish。
    // HTTP 清理后再取消剩余上传，磁盘及 metadata 覆盖迟到结果和尾部清理。
    if (server_ && !httpStopped_) {
        internal::shutdownTrace("http", "stop_requested", this);
        server_->stop(prepared_.configuration.shutdownGraceSeconds, [this] {
            internal::shutdownTrace("http", "stopped", this);
            httpStopped_ = true;
            if (httpStarted_) (void)runtime_.markHttpStopped();
            uploads_.requestStop([this]{disk_.requestStop();stopMetadata();});
            finishIfStopped();
        });
    } else {
        uploads_.requestStop([this]{disk_.requestStop();stopMetadata();});
    }
    stopMetadata();
    finishIfStopped();
}

void InstanceServer::stopMetadata() {
    if(!stopCalled_ || !identity_.stopped() || !uploads_.stopped() || !disk_.snapshot().stopped)return;
    internal::shutdownTrace("instance", "identity_stopped_start_metadata_stop", this);
    runtime_.requestStop();
    if (httpStarted_ && httpStopped_) (void)runtime_.markHttpStopped();
    finishIfStopped();
}

TransferDispatcher& InstanceServer::transferDispatcher() { loop_.assertInLoopThread(); return uploads_; }


IdentityDispatcher& InstanceServer::identityDispatcher() { loop_.assertInLoopThread(); return identity_; }

void InstanceServer::finishIfStopped() {
    if (stopCalled_) internal::shutdownTrace("instance", "barriers", this,
        runtime_.state() == RuntimeState::Stopped, identity_.stopped(), httpStopped_);
    if (completed_ || runtime_.state() != RuntimeState::Stopped || !identity_.stopped() || !uploads_.stopped() || !disk_.snapshot().stopped || !httpStopped_) return;
    completed_ = true;
    internal::shutdownTrace("instance", "all_stopped", this);
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
        auto worker = [database] { return makeInstanceMetadataResources(database); };
        return std::make_unique<InstanceServer>(loop, std::move(prepared),
            InstanceCrypto{ std::move(tokens).value(), std::move(requests).value() }, std::move(worker), makeInstancePasswordResources,
            std::move(httpObservation), std::move(taskObservation));
    } catch (...) { return ErrorCode::InternalError; }
}

}  // namespace anof::runtime
