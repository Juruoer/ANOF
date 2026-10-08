#pragma once

#include <anof/application/ports/SessionTokenCodec.h>
#include <anof/transport/http/JsonResponse.h>
#include <muduox/net/http/HttpHeaders.h>

#include <optional>
#include <variant>

namespace anof::http {

using BearerCredentials = std::variant<SessionDigest, ProtocolError>;
BearerCredentials readBearerCredentials(const muduox::net::HttpHeaders& headers, const SessionTokenCodec& codec);
std::optional<ProtocolError> validateJsonContentHeaders(const muduox::net::HttpHeaders& headers);

}  // namespace anof::http
