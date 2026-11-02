#include <anof/transport/http/IdentityRequest.h>
#include <anof/transport/http/RequestCredentials.h>
#include <anof/transport/http/internal/FlatJson.h>
#include <muduox/net/http/HttpQueryParams.h>
#include <arpa/inet.h>

namespace anof::http {
using internal::FieldType;
using internal::FlatJson;
Parsed<RegistrationRequest> parseRegisterJson(std::string_view body, AuthenticationSource source) {
    FlatJson json{{"login"}, {"display_name"}, {"password"}, {"invite_token"}, {"credential_mode", FieldType::Text, false}};
    if (auto error = json.parse(body)) return *error;
    auto login = LoginName::parse(json[0].text);
    auto display = DisplayName::parse(json[1].text);
    auto password = PasswordInput::parse(json[2].text);
    auto invite = InviteTokenText::parse(json[3].text);
    if (!login || !display || !password || !invite) return ProtocolError::InvalidArgument;
    if (json[4].seen && json[4].text != "bearer" && json[4].text != "cookie") return ProtocolError::InvalidArgument;
    return RegistrationRequest{{std::move(login).value(), std::move(display).value(), std::move(password).value(), std::move(invite).value(), source}, json[4].text == "cookie" ? CredentialMode::Cookie : CredentialMode::Bearer};
}
Parsed<LoginCommand> parseLoginJson(std::string_view body, AuthenticationSource source) {
    FlatJson json{{"login"}, {"password"}, {"credential_mode"}};
    if (auto error = json.parse(body)) return *error;
    auto login = LoginName::parse(json[0].text);
    auto password = PasswordInput::parse(json[1].text);
    if (!login || !password || (json[2].text != "bearer" && json[2].text != "cookie")) return ProtocolError::InvalidArgument;
    return LoginCommand{std::move(login).value(), std::move(password).value(), json[2].text == "cookie" ? CredentialMode::Cookie : CredentialMode::Bearer, source};
}
Parsed<CreateInviteCommand> parseCreateInviteJson(std::string_view body) {
    FlatJson json{{"max_uses", FieldType::Integer, false}, {"valid_for_seconds", FieldType::Integer, false}};
    if (auto error = json.parse(body)) return *error;
    auto uses = InviteUseLimit::from(json[0].seen ? json[0].integer : 1);
    auto lifetime = InviteLifetimeSeconds::from(json[1].seen ? json[1].integer : 604800);
    if (!uses || !lifetime) return ProtocolError::InvalidArgument;
    return CreateInviteCommand{uses.value(), lifetime.value()};
}
Parsed<GetNodeCommand> parseGetNode(std::string_view id) {
    auto value = NodeId::parse(id);
    if (!value) return ProtocolError::InvalidArgument;
    return GetNodeCommand{value.value()};
}
Parsed<RevokeInviteCommand> parseRevokeInvite(std::string_view id) {
    auto value = InviteId::parse(id);
    if (!value) return ProtocolError::InvalidArgument;
    return RevokeInviteCommand{value.value()};
}
std::optional<ProtocolError> validateAnonymousCredentials(const muduox::net::HttpHeaders& headers) {
    if (headers.contains("authorization")) return ProtocolError::InvalidArgument;
    auto cookie = readCookieSession(headers);
    if (auto* error = std::get_if<ProtocolError>(&cookie)) return *error;
    return std::nullopt;
}
std::optional<ProtocolError> validateBodylessHeaders(const muduox::net::HttpHeaders& headers) {
    using Single = muduox::net::HttpHeaders::SingleStatus;
    const auto length = headers.getSingle("content-length");
    if (headers.contains("transfer-encoding") || headers.contains("trailer") || length.status == Single::Multiple) return ProtocolError::InvalidArgument;
    if (length.status == Single::Unique) {
        // HTTP OWS 和前导零由框架定界允许；这里只允许数值零。
        auto value = length.value;
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        if (value.empty() || value.find_first_not_of('0') != std::string_view::npos) return ProtocolError::InvalidArgument;
    }
    return std::nullopt;
}
std::optional<ProtocolError> validateBodylessContent(std::string_view body, const muduox::net::HttpHeaders& trailers) {
    if (!body.empty() || !trailers.empty()) return ProtocolError::InvalidArgument;
    return std::nullopt;
}
std::optional<ProtocolError> validateNoQuery(std::string_view query) {
    const auto parsed = muduox::net::parseHttpQuery(query, muduox::net::HttpQueryMode::Percent, {8192, 0, 0});
    if (std::holds_alternative<muduox::net::HttpQueryError>(parsed)) return ProtocolError::InvalidArgument;
    return std::nullopt;
}
Parsed<AuthenticationSource> readAuthenticationSource(const muduox::net::HttpRequestMetadata& metadata) {
    if (!metadata.connection.peer) return ProtocolError::InternalError;
    const auto& peer = *metadata.connection.peer;
    if (peer.ip.find('\0') != std::string::npos) return ProtocolError::InternalError;
    switch (peer.family) {
    case muduox::net::HttpEndpoint::Family::IPv4: {
        std::array<std::uint8_t, 4> bytes{};
        if (::inet_pton(AF_INET, peer.ip.c_str(), bytes.data()) != 1) return ProtocolError::InternalError;
        return AuthenticationSource::ipv4(bytes);
    }
    case muduox::net::HttpEndpoint::Family::IPv6: {
        std::array<std::uint8_t, 16> bytes{};
        if (::inet_pton(AF_INET6, peer.ip.c_str(), bytes.data()) != 1) return ProtocolError::InternalError;
        return AuthenticationSource::ipv6(bytes);
    }
    }
    return ProtocolError::InternalError;
}
}  // namespace anof::http
