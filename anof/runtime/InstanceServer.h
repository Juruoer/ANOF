#pragma once

#include <anof/application/maintenance/PrepareInstance.h>
#include <anof/runtime/AnofRuntime.h>
#include <anof/runtime/IdentityExecutor.h>
#include <anof/runtime/UploadExecutor.h>
#include <anof/transport/http/CreateDirectoryHandler.h>

#include <memory>
#include <optional>

namespace anof::runtime {

struct InstanceCrypto {
    std::unique_ptr<SessionTokenCodec> tokens;
    std::unique_ptr<RequestIdentity> requests;
};

// 拥有实例 lease、loop 侧 crypto、runtime 和 HTTP；loop 本身由调用者拥有。
// constructor 不绑定端口；worker-ready 后才创建 HttpServer。仅所属 loop 调用。
class InstanceServer {
public:
    InstanceServer(muduox::net::EventLoop& loop, PreparedInstance prepared, InstanceCrypto crypto,
                   MetadataExecutor::Factory worker, IdentityExecutor::Factory passwordWorker, http::HttpObservation httpObservation = {},
                   http::DirectoryTaskObserver taskObservation = {});
    // 已启动时须先到 Stopped，并在自身回调外析构；lease 最后释放。
    ~InstanceServer();
    InstanceServer(const InstanceServer&) = delete;
    InstanceServer& operator=(const InstanceServer&) = delete;

    // 接纳后 ready 恰好一次；启动中取消以 Retryable 通知，不能重启同一对象。
    // 监听失败经 ready 返回分类；仍须等 stopped 并在回调外析构，才能释放实例锁。
    Result<void> start(MetadataExecutor::Started ready = {}, MetadataExecutor::Stopped stopped = {});
    void requestStop();
    RuntimeState state() const;
    bool ready() const;
    std::optional<std::uint16_t> port() const;
    MetadataExecutorSnapshot metadataSnapshot() const;
    std::size_t notificationExceptions() const;
    IdentityDispatcher& identityDispatcher();
    TransferDispatcher& transferDispatcher();

private:
    void initialized(Result<void> result);
    void identityInitialized(Result<void> result);
    void transferInitialized(Result<void>);
    void listen();
    void stopMetadata();
    void notifyReady(Result<void> result);
    void finishIfStopped();
    muduox::net::EventLoop& loop_;
    PreparedInstance prepared_;
    InstanceCrypto crypto_;
    MetadataExecutor::Factory worker_;
    IdentityExecutor::Factory passwordWorker_;
    http::HttpObservation httpObservation_;
    http::DirectoryTaskObserver taskObservation_;
    AnofRuntime runtime_;
    IdentityExecutor identity_;
    DiskExecutor disk_;
    UploadExecutor uploads_;
    std::unique_ptr<http::HttpApi> api_;
    std::unique_ptr<muduox::net::HttpServer> server_;
    MetadataExecutor::Started onReady_;
    MetadataExecutor::Stopped onStopped_;
    std::optional<std::uint16_t> port_;
    bool startCalled_ = false;
    bool stopCalled_ = false;
    bool httpStarted_ = false;
    bool httpStopped_ = true;
    bool completed_ = false;
    std::size_t notificationExceptions_ = 0;
};

// 生产组合：工厂在 loop 初始化 crypto，在 worker 打开 SQLite 并构造 Clock／NodeIdGenerator。
// 仅接纳 prepareInstance 已完成的 lease；不负责获取锁、加载配置或创建实例。
Result<std::unique_ptr<InstanceServer>> makeInstanceServer(muduox::net::EventLoop& loop, PreparedInstance prepared,
    http::HttpObservation httpObservation = {}, http::DirectoryTaskObserver taskObservation = {});

}  // namespace anof::runtime
