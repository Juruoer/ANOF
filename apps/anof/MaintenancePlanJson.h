#pragma once
#include <anof/domain/maintenance/MaintenanceValues.h>

#include <nlohmann/json.hpp>
namespace anof::app {
// 外部计划格式：严格键集合、规范字符串数量/ID、UTF-8、深度和字节上限。
Result<MaintenancePlan> parseMaintenancePlan(std::string_view);
nlohmann::json maintenancePlanData(const MaintenancePlan&);
nlohmann::json maintenanceFindingData(const MaintenanceFinding&);
nlohmann::json maintenanceSummaryData(const MaintenanceReport&);
}  // namespace anof::app
