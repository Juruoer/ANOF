#pragma once

#include <anof/transport/http/JsonResponse.h>

#include <string_view>
#include <variant>

namespace anof::http {

using CreateDirectoryJsonResult = std::variant<CreateDirectoryCommand, ProtocolError>;
CreateDirectoryJsonResult parseCreateDirectoryJson(std::string_view body);

}  // namespace anof::http
