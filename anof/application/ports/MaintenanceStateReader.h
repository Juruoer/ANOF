#pragma once
#include <anof/core/Page.h>
#include <anof/domain/maintenance/MaintenanceValues.h>
namespace anof {
// 普通只读快照内的启动责任查询；不打开检查副本，不提供修复/SQL入口。
class MaintenanceStateReader {
public:
    virtual ~MaintenanceStateReader() = default;
    virtual Result<std::optional<QuarantineEntry>> findQuarantine(QuarantineId) = 0;
    virtual Result<bool> hasPendingRuns() = 0;
};
}  // namespace anof
