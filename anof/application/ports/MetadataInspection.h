#pragma once
#include <anof/application/ports/OfflineInstanceAccess.h>
#include <anof/domain/drive/NodeAccounting.h>
#include <anof/domain/transfer/Storage.h>
#include <anof/domain/transfer/Upload.h>

namespace anof {
// 索引只在一个reader寿命内有效；不是SQL游标或未经验证的where表达式。
using InspectionPosition = Quantity<struct InspectionPositionTag>;
struct InspectedObject {
    BlobRecord record;
    bool nodeReference;
    bool uploadReference;
};
struct InspectionIdentity {
    std::optional<InstanceId> instance;
    std::optional<std::int64_t> schemaVersion, formatVersion;
};
struct AccountingFacts {
    StorageSnapshot recordedStorage;
    ByteCount provenRetained, provenReserved;
    NodeAccounting recordedNodes, provenNodes;
    std::optional<Sha256> provenLedger;  // 完整逻辑/物理/节点账本事实的确定性摘要，供计划绑定。
    bool complete;                       // false不得用于RecomputeAccounting，损坏行同时通过findings报告。
};
// 每个getter（含缓存读取）先校验本次上下文；绑定创建时operationId，阶段为Inspect。
// 取消/错误ID不新增数据库损坏finding，不消费分页位置。返回后解除progress callback。
class MetadataInspectionReader {
public:
    virtual ~MetadataInspectionReader() = default;
    virtual MaintenanceResult<InspectionIdentity> identity(const MaintenanceContext&) = 0;
    virtual MaintenanceResult<Page<MaintenanceFinding, InspectionPosition>> findings(const MaintenanceContext&, PageSize, std::optional<InspectionPosition>) = 0;
    virtual MaintenanceResult<Page<InspectedObject, BlobId>> objects(const MaintenanceContext&, PageSize, std::optional<BlobId>) = 0;
    virtual MaintenanceResult<Page<QuarantineEntry, QuarantineId>> quarantine(const MaintenanceContext&, PageSize, std::optional<QuarantineId>) = 0;
    virtual MaintenanceResult<Page<Upload, UploadId>> uploads(const MaintenanceContext&, PageSize, std::optional<UploadId>) = 0;
    virtual MaintenanceResult<Page<MaintenanceRun, MaintenanceRunId>> runs(const MaintenanceContext&, PageSize, std::optional<MaintenanceRunId>) = 0;
    virtual MaintenanceResult<AccountingFacts> accounting(const MaintenanceContext&) = 0;
    virtual MaintenanceResult<Sha256> repairInvariant(const MaintenanceContext&, const MaintenancePlan&) = 0;
    virtual MaintenanceResult<MaintenanceReport> summary(const MaintenanceContext&) = 0;
};
class MetadataInspection {
public:
    virtual ~MetadataInspection() = default;
    // 专用损坏容忍读取；不经业务opener；只允许在capture副本恢复SQLite，不修业务数据。
    // reader同线程销毁，先于capture释放；返回finding可描述无法构造正常模型的行。
    virtual MaintenanceResult<std::unique_ptr<MetadataInspectionReader>> open(
        const MaintenanceContext&, const InspectionCapture&) = 0;
};
}  // namespace anof
