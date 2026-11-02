#pragma once
#include <anof/application/ports/OfflineInstanceAccess.h>

namespace anof {
// 明确本地操作者，不允许伪造HTTP Principal。事件与动作状态/账本变更共用事务。
enum class MaintenanceAuditKind { PlanStarted,
                                  ActionPrepared,
                                  ActionApplied,
                                  ActionVerified,
                                  PlanCompleted,
                                  PasswordReset };
struct MaintenanceAuditEvent {
    MaintenanceRunId run;
    OperationId operation;
    MaintenanceAuditKind kind;
    MaintenanceOutcome outcome;
    std::optional<MaintenanceError> error;
    std::optional<MaintenanceFileKey> resource;
    std::optional<MaintenanceActionId> action;
    UtcTime occurredAt;
};
// 事务继承session的operationId值，每次调用独立借用上下文；校验失败不得执行本次SQL。
// 任一写调用/commit失败后事务不可再提交，必须回滚/销毁；清理不受取消影响。
// commit入口已取消时不提交；COMMIT开始后保留真实成功/错误/未知结果，不用迟到取消覆盖。
class MetadataRepairTransaction {
public:
    virtual ~MetadataRepairTransaction() = default;  // 未commit回滚；不跨磁盘操作或worker等待。
    virtual MaintenanceResult<void> insertRun(const MaintenanceContext&, const MaintenanceRun&) = 0;
    virtual MaintenanceResult<void> prepareAction(const MaintenanceContext&, MaintenanceRunId, const MaintenanceActionRecord&) = 0;
    // 仅Prepared->Applied->Verified，每次推进revision+1；复验run/plan摘要/实例/action及expected绑定。
    // 证据来自持同一lease的文件适配器，事务内不做文件I/O。持久化完整typed事实而非仅摘要。
    // 核对动作特定事实及同事务元数据后置条件；Applied不等于Verified，未知证据禁止推进。
    virtual MaintenanceResult<void> advanceAction(const MaintenanceContext&, MaintenanceRunId, MaintenanceActionId,
                                                  Revision expected, MaintenanceActionState, const MaintenanceActionEvidence&, UtcTime) = 0;
    virtual MaintenanceResult<void> completeRun(const MaintenanceContext&, MaintenanceRunId, Revision expected, UtcTime) = 0;
    virtual MaintenanceResult<void> markUnavailable(const MaintenanceContext&, const MarkBlobUnavailable&, const MaintenanceActionEvidence&, UtcTime) = 0;
    virtual MaintenanceResult<void> restoreAvailable(const MaintenanceContext&, const VerifyRestoredBlob&, const MaintenanceActionEvidence&, UtcTime) = 0;
    // 对所有可解释逻辑/物理/节点账本整体重算；provenLedger必须匹配同一事务重算的事实。
    virtual MaintenanceResult<void> recomputeAccounting(const MaintenanceContext&, const RecomputeAccounting&) = 0;
    // prepare同时增加P；held只转状态，不能重复计费；没有自动删除隔离/释放charge入口。
    virtual MaintenanceResult<void> prepareQuarantine(const MaintenanceContext&, const QuarantineEntry&) = 0;
    virtual MaintenanceResult<void> holdQuarantine(const MaintenanceContext&, QuarantineId, Revision expected, const MaintenanceActionEvidence&, UtcTime) = 0;
    virtual MaintenanceResult<void> appendAudit(const MaintenanceContext&, const MaintenanceAuditEvent&) = 0;
    virtual MaintenanceResult<void> commit(const MaintenanceContext&) = 0;
};
// 返回事务不继承任何取消借用；session必须活得比事务长。方法阶段使用Recover。
class MetadataRepairSession {
public:
    virtual ~MetadataRepairSession() = default;
    virtual MaintenanceResult<std::optional<MaintenanceRun>> findRun(const MaintenanceContext&, MaintenancePlanId) = 0;
    virtual MaintenanceResult<Page<MaintenanceActionRecord, MaintenanceActionIndex>> actions(const MaintenanceContext&, MaintenanceRunId, PageSize, std::optional<MaintenanceActionIndex>) = 0;
    virtual MaintenanceResult<void> verifyCompletedAction(const MaintenanceContext&, MaintenanceRunId, MaintenanceActionId, const MaintenanceActionEvidence&) = 0;
    virtual MaintenanceResult<std::unique_ptr<MetadataRepairTransaction>> beginWrite(const MaintenanceContext&) = 0;
};
class MetadataRepair {
public:
    virtual ~MetadataRepair() = default;
    // 调用方先验证完整计划、源证据并发布原始备份；session不恢复未完成run、不建库。
    // 一次仅一事务；提交未知后关闭session，重开新连接读run/action证据，不声称已回滚。
    virtual MaintenanceResult<std::unique_ptr<MetadataRepairSession>> open(
        const MaintenanceContext&, const InstanceLease&) = 0;
};
}  // namespace anof
