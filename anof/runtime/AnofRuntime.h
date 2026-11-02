#pragma once

#include <anof/runtime/MetadataExecutor.h>

namespace anof::runtime {

enum class RuntimeState { Starting,
                          Ready,
                          Stopping,
                          Stopped };

// 调度与生命周期基础，不拥有 HTTP 或实例资源；具体装配由 InstanceServer 完成。
// 调用方完成实例准备后 start；HTTP 配置并启动成功后 markHttpStarted。
class AnofRuntime {
public:
    explicit AnofRuntime(muduox::net::EventLoop& loop, MetadataExecutorOptions options = {});
    // 已启动时须先到 Stopped；只能在自身回调之外销毁，loop 必须继续存活。
    ~AnofRuntime();
    AnofRuntime(const AnofRuntime&) = delete;
    AnofRuntime& operator=(const AnofRuntime&) = delete;

    Result<void> start(MetadataExecutor::Factory factory, MetadataExecutor::Started metadataStarted = {},
                       MetadataExecutor::Stopped stopped = {});
    bool markHttpStarted();
    void requestStop();      // 先关闭准入；调用方随后执行 HttpServer::stop。
    bool markHttpStopped();  // 仅由 HTTP stop callback 或启动回滚的等价清理屏障调用。
    RuntimeState state() const;
    bool ready() const;
    CreateDirectoryDispatcher& dispatcher();
    MetadataExecutor& metadataExecutor() { return metadata_; }
    MetadataExecutorSnapshot metadataSnapshot() const;
    std::size_t notificationExceptions() const;

private:
    void finishIfStopped();
    muduox::net::EventLoop& loop_;
    MetadataExecutor metadata_;
    RuntimeState state_ = RuntimeState::Starting;
    bool started_ = false;
    bool metadataReady_ = false;
    bool metadataStopped_ = false;
    bool httpStarted_ = false;
    bool httpStopped_ = false;
    MetadataExecutor::Stopped onStopped_;
    std::size_t notificationExceptions_ = 0;
};

}  // namespace anof::runtime
