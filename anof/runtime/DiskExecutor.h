#pragma once

#include <anof/application/ports/TaskDispatcher.h>
#include <anof/application/ports/TransferFileStore.h>

#include <functional>
#include <memory>

namespace muduox::net {
class EventLoop;
}
namespace anof::runtime {
struct DiskExecutorOptions {
    std::size_t normalPending = 256;
    std::size_t cleanupPending = 8;
};
struct DiskExecutorSnapshot {
    std::size_t pending, running, outstanding;
    bool ready, stopped;
};
// 两个线程；工厂和文件存储均在线程内创建/销毁。独占文件句柄由协调器串行移交。
class DiskExecutor {
public:
    using Factory = std::function<Result<std::unique_ptr<TransferFileStore>>()>;
    using Operation = std::function<Result<void>(TransferFileStore&)>;
    using Started = std::function<void(Result<void>)>;
    using Stopped = std::function<void()>;
    explicit DiskExecutor(muduox::net::EventLoop&, DiskExecutorOptions = {});
    ~DiskExecutor();
    DiskExecutor(const DiskExecutor&) = delete;
    DiskExecutor& operator=(const DiskExecutor&) = delete;
    Result<void> start(Factory, Started, Stopped);
    TaskSubmission submit(Operation, Completion<void>, bool cleanup = false, std::size_t lane = 0);
    // 停止接纳；已接纳任务仍执行并交付。协调器须先停止生产并退休跨阶段工作。
    void requestStop();
    DiskExecutorSnapshot snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace anof::runtime
