#pragma once
#include <anof/domain/maintenance/MaintenanceValues.h>
namespace anof {
class MaintenanceIdGenerator {
public:
    virtual ~MaintenanceIdGenerator() = default;
    virtual Result<MaintenanceRunId> nextRun() = 0;
    virtual Result<MaintenancePlanId> nextPlan() = 0;
    virtual Result<MaintenanceActionId> nextAction() = 0;
    virtual Result<QuarantineId> nextQuarantine() = 0;
    virtual Result<BackupId> nextBackup() = 0;
};
}  // namespace anof
