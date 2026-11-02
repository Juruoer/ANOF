#pragma once
#include <anof/domain/maintenance/MaintenanceValues.h>

#include <nlohmann/json.hpp>
namespace anof::infrastructure::sqlite::maintenance {
using Json = nlohmann::json;
Json encode(const FileEvidence&);
FileEvidence fileEvidence(const Json&);
Json encode(const MaintenanceActionPayload&);
MaintenanceActionPayload payload(const Json&);
Json encode(const MaintenanceActionEvidence&);
MaintenanceActionEvidence evidence(const Json&);
Json encode(const QuarantineEntry&);
QuarantineEntry quarantine(const Json&);
// 内部持久化编码；不作为外部计划/清单解析器。
}  // namespace anof::infrastructure::sqlite::maintenance
