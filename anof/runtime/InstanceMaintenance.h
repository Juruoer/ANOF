#pragma once
#include <anof/application/maintenance/Backup.h>
#include <anof/domain/identity/Secrets.h>
namespace anof::runtime {
// 每个离线操作线程独立持有适配器；不与在线worker或HTTP共享连接。
class InstanceMaintenance {
public:
    InstanceMaintenance();
    ~InstanceMaintenance();
    FsckService& fsck();
    BackupService& backups();
    MaintenanceResult<InstanceConfiguration> configuration(const MaintenanceContext&, const std::filesystem::path&);
    MaintenanceResult<void> resetAdministrator(const MaintenanceContext&, const std::filesystem::path&, const PasswordInput&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace anof::runtime
