#pragma once
#include <anof/application/ports/InstancePreparation.h>
#include <anof/application/ports/MaintenanceResult.h>

#include <atomic>

namespace anof {
// 同步调用借用context和取消标志，包括progress callback；退出调用前必须解除所有借用。
// 返回的Reader/Session/Transaction/Inventory/Backup只可按值保存operationId，不可保存context或取消引用。
// 后续方法传入相同operationId，可使用新的取消标志。析构/回滚不得依赖调用上下文存活。
struct MaintenanceContext {
    OperationId operationId;
    const std::atomic<bool>& cancelled;
};

// 仅用于调用入口、任何工作/副作用之前：先检查操作绑定，再检查本次取消。
// NotStarted只说明本次方法未开始，不代表此前调用的修改已回滚；失败携带本次传入的operationId。
// 不得在commit/rename等发生后用此检查覆盖真实成功、部分效果或Unknown；结果以实际效果为准。
inline MaintenanceResult<void> validateMaintenanceCall(
    const MaintenanceContext& call, OperationId expected, MaintenanceStage stage) {
    if (call.operationId != expected)
        return MaintenanceFailure{ MaintenanceError::InvalidArgument, stage, call.operationId, MaintenanceOutcome::NotStarted, MaintenanceReportStatus::Incomplete, {}, {} };
    if (call.cancelled.load())
        return MaintenanceFailure{ MaintenanceError::Cancelled, stage, call.operationId, MaintenanceOutcome::NotStarted, MaintenanceReportStatus::Incomplete, {}, {} };
    return {};
}
// capture只有原样文件复制；源SQLite不得打开。clone可恢复sidecar，再query_only检查。
// lease必须活得比scratch及其所有reader长；scratch析构只清理本次拥有的外部临时目录。
class InspectionCapture {
public:
    virtual ~InspectionCapture() = default;
    virtual const std::filesystem::path& root() const noexcept = 0;
    virtual const MaintenanceFingerprint& fingerprint() const noexcept = 0;
    virtual const Sha256& configurationFingerprint() const noexcept = 0;
};
// 安全定位父目录、旁路锁和本次staging；析构释放锁但保留未发布产物供诊断，不删除锁文件。
class MaintenanceDestination {
public:
    virtual ~MaintenanceDestination() = default;
    virtual const std::filesystem::path& target() const noexcept = 0;
    virtual const std::filesystem::path& staging() const noexcept = 0;
};
class OfflineInstanceAccess {
public:
    virtual ~OfflineInstanceAccess() = default;
    // 非阻塞，仅既有锁；损坏数据库不影响取得lease，绝不创建缺失实例。
    virtual MaintenanceResult<std::unique_ptr<InstanceLease>> acquireExisting(
        const MaintenanceContext&, const std::filesystem::path&) = 0;
    virtual MaintenanceResult<std::unique_ptr<InspectionCapture>> captureInspection(
        const MaintenanceContext&, const InstanceLease&, const std::filesystem::path& externalWorkDirectory) = 0;
    // source与目标不得祖先/后代重叠；拒绝已有目标。多个锁由调用方按规范绝对路径排序取得。
    virtual MaintenanceResult<std::unique_ptr<MaintenanceDestination>> acquireDestination(
        const MaintenanceContext&, const std::filesystem::path& source, const std::filesystem::path& target) = 0;
};
}  // namespace anof
