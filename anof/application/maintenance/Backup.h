#pragma once
#include <anof/application/maintenance/Fsck.h>
#include <anof/application/ports/MetadataSnapshot.h>
namespace anof {
struct BackupRequest {
    std::filesystem::path root, output, workDirectory;
    bool excludeQuarantine = false;
};
struct RestoreRequest {
    std::filesystem::path backup, root;
    InstanceConfiguration configuration;
};
struct BackupResult {
    BackupManifest manifest;
    std::filesystem::path artifact;
};
struct RestoreResult {
    InstanceId instance;
    std::filesystem::path artifact;
};
class BackupService {
public:
    BackupService(OfflineInstanceAccess&, MetadataInspection&, MetadataSnapshot&, MaintenanceFileStore&,
                  InstanceConfigurationLoader&, MaintenanceRecovery&, MaintenanceIdGenerator&, Clock&, FsckService&);
    MaintenanceResult<BackupResult> backup(const MaintenanceContext&, const BackupRequest&);
    MaintenanceResult<RestoreResult> restore(const MaintenanceContext&, const RestoreRequest&);

private:
    OfflineInstanceAccess& access_;
    MetadataInspection& inspection_;
    MetadataSnapshot& snapshot_;
    MaintenanceFileStore& files_;
    InstanceConfigurationLoader& configuration_;
    MaintenanceRecovery& recovery_;
    MaintenanceIdGenerator& ids_;
    Clock& clock_;
    FsckService& fsck_;
};
}  // namespace anof
