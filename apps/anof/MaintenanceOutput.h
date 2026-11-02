#pragma once
#include <anof/application/ports/MaintenanceResult.h>

#include <nlohmann/json.hpp>

namespace anof::app {
// 本地运维结果独立于远程客户端结果；仅此本地输出允许带产物路径。
int maintenanceExitCode(const MaintenanceFailure&) noexcept;
const char* maintenanceErrorName(MaintenanceError) noexcept;
const char* maintenanceStageName(MaintenanceStage) noexcept;
const char* maintenanceOutcomeName(MaintenanceOutcome) noexcept;
const char* maintenanceStatusName(MaintenanceReportStatus) noexcept;
nlohmann::json maintenanceFailureData(const MaintenanceFailure&);
}  // namespace anof::app
