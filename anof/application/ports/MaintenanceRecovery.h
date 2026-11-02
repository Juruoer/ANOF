#pragma once
#include <anof/application/ports/OfflineInstanceAccess.h>
namespace anof {
// 仅离线编排持锁调用，执行单个已持久化意图；不能隐式恢复其他对象或开启周期任务。
class MaintenanceRecovery {
public:
    virtual ~MaintenanceRecovery() = default;
    virtual MaintenanceResult<void> resume(const MaintenanceContext&, const InstanceLease&, const ResumeUpload&) = 0;
    // 先删除/同步，APPLIED仍保留DELETING责任；其后才结算，允许重试确认已完成结算。
    virtual MaintenanceResult<void> remove(const MaintenanceContext&, const InstanceLease&, const ResumeDeletingBlob&) = 0;
    virtual MaintenanceResult<void> finalize(const MaintenanceContext&, const InstanceLease&, const ResumeDeletingBlob&) = 0;
};
}  // namespace anof
