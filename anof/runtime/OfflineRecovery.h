#pragma once
#include <anof/application/ports/MaintenanceRecovery.h>

#include <memory>
namespace anof::runtime {
// 同步离线装配：由维护调用线程持锁执行，不创建HTTP loop或周期清理任务。
std::unique_ptr<MaintenanceRecovery> makeOfflineMaintenanceRecovery();
}  // namespace anof::runtime
