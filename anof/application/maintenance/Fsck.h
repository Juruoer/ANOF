#pragma once
#include <anof/application/ports/MaintenanceDigest.h>
#include <anof/application/ports/MaintenanceFileStore.h>
#include <anof/application/ports/MaintenanceIdGenerator.h>
#include <anof/application/ports/MaintenanceRecovery.h>
#include <anof/application/ports/MetadataInspection.h>
#include <anof/application/ports/MetadataRepair.h>

#include <functional>
namespace anof {
// 同步离线用例，不可在HTTP/I/O loop调用；只借用端口，方法退出后不保留context或lease。
struct FsckRequest {
    std::filesystem::path root, workDirectory;
    InspectionMode mode = InspectionMode::Quick;
    bool plan = false;
};
struct FsckResult {
    InspectionIdentity identity;
    MaintenanceFingerprint source;
    MaintenanceReport report;
    std::optional<MaintenancePlan> plan;
};
// 有界逐条输出，最多10000条；失败必须终止，不把未写出的报告称为完整。
using FindingSink = std::function<MaintenanceResult<void>(const MaintenanceFinding&)>;
struct ApplyRepairRequest {
    std::filesystem::path root, workDirectory, originalBackup;
    bool confirm = false;
};
struct ApplyRepairResult {
    MaintenanceRunId run;
    bool alreadyCompleted;
    FsckResult verification;
};
class FsckService {
public:
    FsckService(OfflineInstanceAccess&, MetadataInspection&, MaintenanceFileStore&, MetadataRepair&,
                MaintenanceRecovery&, MaintenanceDigest&, MaintenanceIdGenerator&, Clock&);
    MaintenanceResult<FsckResult> inspect(const MaintenanceContext&, const FsckRequest&, const FindingSink& = {});
    MaintenanceResult<ApplyRepairResult> apply(const MaintenanceContext&, const ApplyRepairRequest&,
                                               const MaintenancePlan&, const FindingSink& = {});

private:
    friend class BackupService;
    OfflineInstanceAccess& access_;
    MetadataInspection& inspection_;
    MaintenanceFileStore& files_;
    MetadataRepair& repair_;
    MaintenanceRecovery& recovery_;
    MaintenanceDigest& digest_;
    MaintenanceIdGenerator& ids_;
    Clock& clock_;
    MaintenanceResult<FsckResult> inspectHeld(const MaintenanceContext&, const InstanceLease&, const FsckRequest&,
                                              const FindingSink&);
};
// 内部确定性编码用于摘要/语义比较，不是外部JSON解码，也不包含密码/路径/SQL。
std::string maintenanceActionBytes(const MaintenanceActionPayload&);
std::string maintenancePlanBytes(const MaintenancePlan&);
}  // namespace anof
