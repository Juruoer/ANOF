#pragma once
#include <anof/domain/maintenance/MaintenanceValues.h>

#include <nlohmann/json.hpp>
namespace anof::infrastructure::storage::maintenance {
using Json = nlohmann::json;
Result<Json> parseStrict(std::string_view, std::size_t);
Json encode(const BackupManifest&);
Result<BackupManifest> manifest(const Json&);
Json encode(const Blob&);
Result<Blob> object(const Json&);
}  // namespace anof::infrastructure::storage::maintenance
