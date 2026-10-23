#pragma once
#include <anof/runtime/BlobAccessRegistry.h>
#include <anof/runtime/DiskExecutor.h>
#include <anof/runtime/MetadataExecutor.h>
namespace anof::runtime {
// 单metadata与既有disk lane；无额外线程。每tick最多16过期项/16新GC/16重试，串行一条GC链。
class MaintenanceExecutor {
public:
    MaintenanceExecutor(muduox::net::EventLoop&, MetadataExecutor&, DiskExecutor&, BlobAccessRegistry&);
    ~MaintenanceExecutor();
    void recover(std::function<void(Result<void>)>);
    void start();
    void tick(); // 与60秒单调定时器相同的有界入口，也供确定性测试调用。
    void requestStop(std::function<void()>);
    bool stopped() const;
    bool busy() const;
    bool paused() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
