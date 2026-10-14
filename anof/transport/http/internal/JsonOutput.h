#pragma once

#include <anof/transport/http/JsonResponse.h>
#include <nlohmann/json.hpp>

namespace anof::http::internal {
JsonResponse jsonResponse(int status, const char* reason, RequestId requestId, const nlohmann::json& body, std::size_t limit = kMaxJsonBytes);
}  // namespace anof::http::internal
