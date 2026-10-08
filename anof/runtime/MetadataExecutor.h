#pragma once

#include <anof/application/ports/CreateDirectoryDispatcher.h>

#include <cstddef>
#include <functional>
#include <memory>

namespace muduox::net {
class EventLoop;
}

namespace anof::runtime {

// 工厂在 worker 调用；所有成员也在 worker 销毁，禁止提前打开连接后跨线程搬运。
struct MetadataWorkerResources {
    std::unique_ptr<MetadataStore> metadata;
    std::unique_ptr<Clock> clock;
    std::unique_ptr<NodeIdGenerator> nodeIds;
};

struct MetadataExecutorOptions {
    std::size_t maxPending = 256;
    std::size_t drainBatch = 32;
};

enum class MetadataExecutorState { Created, Starting, Ready, Stopping, Stopped };

struct MetadataExecutorSnapshot {
    MetadataExecutorState state;
    std::size_t pending;
    std::size_t completed;
    std::size_t outstanding;
    bool running;
    std::size_t taskExceptions;
    std::size_t callbackExceptions;
    std::size_t postFailures;
};

class MetadataExecutor final : public CreateDirectoryDispatcher {
public:
    using Factory = std::function<Result<MetadataWorkerResources>()>;
    using Started = std::function<void(Result<void>)>;
    using Stopped = std::function<void()>;

    explicit MetadataExecutor(muduox::net::EventLoop& loop, MetadataExecutorOptions options = {});
    // 必须先 requestStop，保持 loop 运行直至 Stopped；不能在自身回调内销毁。
    // 不在析构中隐式阻塞 join，不 detach。loop 比本对象活得更久。
    ~MetadataExecutor() override;
    MetadataExecutor(const MetadataExecutor&) = delete;
    MetadataExecutor& operator=(const MetadataExecutor&) = delete;

    // 均在所属 loop 调用。成功接纳启动后，started 延后一次；失败启动也触发 stopped。
    // 同步拒绝（空工厂、重复启动、线程创建失败）不调用回调。
    Result<void> start(Factory factory, Started started = {}, Stopped stopped = {});
    CreateDirectorySubmission trySubmit(RequestContext context, CreateDirectoryCommand command,
                                        CreateDirectoryCallback completion) override;
    void requestStop();
    MetadataExecutorSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anof::runtime
