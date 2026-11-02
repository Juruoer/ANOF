#include <anof/transport/http/RequestCredentials.h>
#include <anof/domain/identity/IdentityValues.h>
#include <set>

namespace anof::http {
namespace {
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}
bool equalAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}
}  // namespace

CookieSession readCookieSession(const muduox::net::HttpHeaders& headers) {
    using Single = muduox::net::HttpHeaders::SingleStatus;
    const auto field = headers.getSingle("Cookie");
    if (field.status == Single::Missing) return std::optional<std::string_view>{};
    if (field.status != Single::Unique || field.value.size() > 8192) return ProtocolError::InvalidArgument;
    std::set<std::string_view> names;
    std::optional<std::string_view> session;
    auto rest = field.value;
    // OWS 可含 HTAB，但值内部及其他控制字节不可进入 Cookie。
    for (unsigned char c : rest) if ((c < 32 && c != '\t') || c >= 127) return ProtocolError::InvalidArgument;
    do {
        const auto separator = rest.find(';');
        auto pair = trim(rest.substr(0, separator));
        const auto equals = pair.find('=');
        if (equals == std::string_view::npos || equals == 0) return ProtocolError::InvalidArgument;
        auto name = pair.substr(0, equals), value = pair.substr(equals + 1);
        for (unsigned char c : name) {
            const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (!alpha && std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) == std::string_view::npos) return ProtocolError::InvalidArgument;
        }
        for (unsigned char c : value) if (c <= 32 || c >= 127) return ProtocolError::InvalidArgument;
        if (!names.insert(name).second || names.size() > 64) return ProtocolError::InvalidArgument;
        if (name == "__Host-anof_session") session = value;
        if (separator == std::string_view::npos) break;
        rest.remove_prefix(separator + 1);
    } while (true);
    return session;
}

std::variant<RequestCredentials, ProtocolError> readRequestCredentials(const muduox::net::HttpHeaders& headers, const SessionTokenCodec& codec) {
    using Single = muduox::net::HttpHeaders::SingleStatus;
    const auto auth = headers.getSingle("authorization");
    auto cookie = readCookieSession(headers);
    if (auto* error = std::get_if<ProtocolError>(&cookie)) return *error;
    auto session = std::get<std::optional<std::string_view>>(cookie);
    if (auth.status == Single::Multiple || (auth.status != Single::Missing && session)) return ProtocolError::InvalidArgument;
    if (session) {
        if (!SecretToken::session(*session)) return ProtocolError::Unauthenticated;
        auto digest = codec.digestBearer(*session);
        if (!digest) return digest.error().code == ErrorCode::Unauthenticated ? ProtocolError::Unauthenticated : ProtocolError::InternalError;
        return RequestCredentials{digest.value(), CredentialMode::Cookie};
    }
    if (auth.status != Single::Unique) return ProtocolError::Unauthenticated;
    auto value = trim(auth.value);
    const auto split = value.find(' ');
    if (split == std::string_view::npos || !equalAscii(value.substr(0, split), "Bearer")) return ProtocolError::Unauthenticated;
    value.remove_prefix(split);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    if (value.empty() || value.find_first_of(" \t\r\n,") != std::string_view::npos) return ProtocolError::Unauthenticated;
    // 编码规范与摘要算法属于注入端口；只传 token，不复制或记录凭据。
    const auto digest = codec.digestBearer(value);
    if (!digest) return digest.error().code == ErrorCode::Unauthenticated ? ProtocolError::Unauthenticated : ProtocolError::InternalError;
    return RequestCredentials{digest.value(), CredentialMode::Bearer};
}

std::optional<ProtocolError> validateJsonContentHeaders(const muduox::net::HttpHeaders& headers) {
    using Single = muduox::net::HttpHeaders::SingleStatus;
    const auto type = headers.getSingle("content-type");
    const auto encoding = headers.getSingle("content-encoding");
    if (type.status == Single::Multiple || encoding.status == Single::Multiple) return ProtocolError::InvalidArgument;
    if (type.status != Single::Unique) return ProtocolError::UnsupportedMediaType;
    if (encoding.status == Single::Unique && !equalAscii(trim(encoding.value), "identity")) return ProtocolError::UnsupportedMediaType;
    auto value = trim(type.value);
    const auto semicolon = value.find(';');
    if (!equalAscii(trim(value.substr(0, semicolon)), "application/json")) return ProtocolError::UnsupportedMediaType;
    if (semicolon == std::string_view::npos) return std::nullopt;
    value = trim(value.substr(semicolon + 1));
    const auto equals = value.find('=');
    if (equals == std::string_view::npos || !equalAscii(trim(value.substr(0, equals)), "charset")) return ProtocolError::UnsupportedMediaType;
    value = trim(value.substr(equals + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    if (!equalAscii(value, "utf-8")) return ProtocolError::UnsupportedMediaType;
    return std::nullopt;
}
}  // namespace anof::http
