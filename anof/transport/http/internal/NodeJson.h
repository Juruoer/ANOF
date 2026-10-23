#pragma once
#include <anof/domain/drive/NodeDetails.h>
#include <nlohmann/json.hpp>
namespace anof::http::internal {
Result<nlohmann::json> nodeJson(const NodeDetails&);
}
