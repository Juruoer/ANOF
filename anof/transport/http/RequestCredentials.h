#pragma once

#include <anof/application/ports/SessionTokenCodec.h>
#include <anof/transport/http/JsonResponse.h>
#include <muduox/net/http/HttpHeaders.h>

#include <optional>
#include <variant>

namespace anof::http {

// Cookie 值仅同步借用；调用方不得跨异步边界保存该视图。
using CookieSession = std::variant<std::optional<std::string_view>, ProtocolError>;
CookieSession readCookieSession(const muduox::net::HttpHeaders&);
struct RequestCredentials {
    SessionDigest digest;
    CredentialMode mode;
};
std::variant<RequestCredentials, ProtocolError> readRequestCredentials(const muduox::net::HttpHeaders& headers, const SessionTokenCodec& codec);
std::optional<ProtocolError> validateJsonContentHeaders(const muduox::net::HttpHeaders& headers);

}  // namespace anof::http
