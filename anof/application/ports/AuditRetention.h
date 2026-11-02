#pragma once
#include <anof/application/ports/MaintenanceResult.h>

namespace anof {
enum class RetainedAuditKind { Identity,
                               Drive,
                               Transfer,
                               Maintenance };
struct AuditRetentionRequest {
    UtcTime before;
    AuditBatchSize totalLimit = AuditBatchSize::from(200).value();
};
struct AuditRetentionResult {
    NodeCount removed;
    bool more;
};
class AuditRetention {
public:
    virtual ~AuditRetention() = default;
    // 同一事务按time/id稳定顺序轮转各类，总删除量<=totalLimit，不是每表各一次limit。
    // 仅事件表；不能删除quarantine/run/action等文件责任。
    virtual Result<AuditRetentionResult> eraseExpired(const AuditRetentionRequest&) = 0;
    // 独立有界事务：固定90天，仅completed且全部动作Verified、无任何文件/隔离责任的run及其action。
    // 时钟回拨不能提前清理，引用存在即跳过；不得使用可配置审计保留期缩短该期限。
    virtual Result<AuditRetentionResult> eraseCompletedRuns(UtcTime now, AuditBatchSize) = 0;
};
}  // namespace anof
