#pragma once
#include <anof/application/ports/OfflineInstanceAccess.h>

namespace anof {
class MetadataSnapshot {
public:
    virtual ~MetadataSnapshot() = default;
    // 源持独占lease，先完成已知责任恢复；输出到本运行staging，不能打开任意已有数据库写入。
    virtual MaintenanceResult<void> exportConsistent(const MaintenanceContext&, const InstanceLease&,
                                                     const MaintenanceDestination&) = 0;
    // 只改副本：删除session/upload/invite/unreferenced/quarantine/run，重算账本，保留历史审计。
    virtual MaintenanceResult<BackupExclusions> normalizeBackup(const MaintenanceContext&,
                                                                const MaintenanceDestination&) = 0;
    // 完整语义验证，关闭checkpointed DELETE副本且确认无sidecar后才返回最终摘要。
    virtual MaintenanceResult<BackupFileDigest> verifyBackup(const MaintenanceContext&,
                                                             const MaintenanceDestination&) = 0;
    // restore仅在本次新staging；复验无有效凭据，切WAL并关闭。不修改业务或实例ID。
    virtual MaintenanceResult<void> prepareRestore(const MaintenanceContext&, const MaintenanceDestination&,
                                                   InstanceId) = 0;
};
}  // namespace anof
