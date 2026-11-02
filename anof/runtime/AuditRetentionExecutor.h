#pragma once
#include <anof/application/ports/AuditRetention.h>
#include <anof/runtime/MetadataExecutor.h>
namespace anof::runtime {
class AuditRetentionExecutor {
public:
    AuditRetentionExecutor(muduox::net::EventLoop&, MetadataExecutor&, AuditRetentionDays, AuditBatchSize);
    ~AuditRetentionExecutor();
    void start();
    void tick();
    void requestStop(std::function<void()> = {});
    bool stopped() const;
    bool paused() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}  // namespace anof::runtime
