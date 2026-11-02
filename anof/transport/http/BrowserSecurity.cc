#include <anof/transport/http/BrowserSecurity.h>
#include <anof/transport/http/RequestCredentials.h>
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <stdexcept>
namespace anof::http {
namespace {
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
Parsed<AuthenticationSource> forwarded(std::string_view raw) {
    std::string ip(raw);
    std::array<char, INET6_ADDRSTRLEN> text{};
    std::array<std::uint8_t, 16> bytes{};
    for (auto family : {AF_INET, AF_INET6}) {
        if (::inet_pton(family, ip.c_str(), bytes.data()) != 1) continue;
        if (!::inet_ntop(family, bytes.data(), text.data(), static_cast<socklen_t>(text.size())) || raw != text.data()) return ProtocolError::InvalidArgument;
        if (family == AF_INET6) return AuthenticationSource::ipv6(bytes);
        std::array<std::uint8_t, 4> v4{};
        std::copy_n(bytes.begin(), 4, v4.begin());
        return AuthenticationSource::ipv4(v4);
    }
    return ProtocolError::InvalidArgument;
}
}  // namespace

BrowserSecurity::BrowserSecurity(const InstanceConfiguration& config)
    : development_(config.developmentHttp), web_(config.webEnabled), origin_(config.publicOrigin), proxies_(config.trustedProxies) {
    if (!isTrustedLoopbackProxy(config.listenAddress)) throw std::invalid_argument("HTTP security requires loopback listener");
    if (development_) {
        if (web_ || !origin_.empty() || !proxies_.empty()) throw std::invalid_argument("Invalid development HTTP security policy");
    } else {
        auto origin = PublicOrigin::parse(origin_);
        if (!origin || origin.value().value() != origin_ || proxies_.empty() || proxies_.size() > 2) throw std::invalid_argument("Invalid formal HTTP security policy");
        authority_ = origin_.substr(8);
        for (std::size_t i = 0; i < proxies_.size(); ++i)
            if (!isTrustedLoopbackProxy(proxies_[i]) || (i && proxies_[0] == proxies_[i])) throw std::invalid_argument("Invalid trusted proxy policy");
    }
}
Parsed<BrowserRequest> BrowserSecurity::inspect(const muduox::net::HttpRequest& request, const muduox::net::HttpRequestMetadata& metadata) const {
    using Single = muduox::net::HttpHeaders::SingleStatus;
    using Request = muduox::net::HttpRequest;
    const auto& h = request.headers();
    auto cookie = readCookieSession(h);
    if (auto* error = std::get_if<ProtocolError>(&cookie)) return *error;
    const auto session = std::get<std::optional<std::string_view>>(cookie);
    const auto auth = h.getSingle("Authorization"), host = h.getSingle("Host"), origin = h.getSingle("Origin"), csrf = h.getSingle("X-ANOF-Request");
    if (auth.status == Single::Multiple || host.status == Single::Multiple || origin.status == Single::Multiple || csrf.status == Single::Multiple ||
        (auth.status != Single::Missing && session)) return ProtocolError::InvalidArgument;
    auto source = readAuthenticationSource(metadata);
    if (auto* error = std::get_if<ProtocolError>(&source)) return *error;
    const bool anonymous = request.method() == Request::kPost && (request.path() == "/api/v1/auth/sessions" || request.path() == "/api/v1/auth/register");
    if (anonymous && auth.status != Single::Missing) return ProtocolError::InvalidArgument;
    if (h.contains("Forwarded") || h.contains("X-Real-IP")) return ProtocolError::InvalidArgument;
    for (const auto& field : h) if (field.name.substr(0, 12) == "x-forwarded-" && field.name != "x-forwarded-for" && field.name != "x-forwarded-proto") return ProtocolError::InvalidArgument;
    const auto ip = h.getSingle("X-Forwarded-For"), proto = h.getSingle("X-Forwarded-Proto");
    const bool trusted = !development_ && metadata.connection.peer && std::find(proxies_.begin(), proxies_.end(), metadata.connection.peer->ip) != proxies_.end();
    if (!trusted && (ip.status != Single::Missing || proto.status != Single::Missing)) return ProtocolError::InvalidArgument;
    if (!development_) {
        if (trusted) {
            if (ip.status != Single::Unique || proto.status != Single::Unique || proto.value != "https") return ProtocolError::InvalidArgument;
            source = forwarded(ip.value);
            if (auto* error = std::get_if<ProtocolError>(&source)) return *error;
        }
        if (!trusted || host.status != Single::Unique || host.value != authority_) return ProtocolError::RequestOriginRejected;
    }
    if (session && !anonymous && !web_) return ProtocolError::CookieModeDisabled;
    const bool unsafe = request.method() != Request::kGet && request.method() != Request::kHead;
    const bool protection = unsafe && ((session && !anonymous) || origin.status != Single::Missing || csrf.status != Single::Missing);
    bool same = false;
    if (origin.status != Single::Missing) {
        if (origin.value.find(',') != std::string_view::npos || origin.value.find(' ') != std::string_view::npos) return ProtocolError::InvalidArgument;
        auto parsed = PublicOrigin::parse(origin.value);
        if (!parsed || origin_.empty() || parsed.value().value() != origin_) return ProtocolError::RequestOriginRejected;
        same = true;
    }
    if (protection && !same) return ProtocolError::RequestOriginRejected;
    if (protection && (csrf.status != Single::Unique || trim(csrf.value) != "1")) return ProtocolError::CsrfRequired;
    auto state = std::make_shared<CookieResponseState>();
    const bool logout = request.path() == "/api/v1/auth/session" && request.method() == Request::kDelete;
    const bool password = request.path() == "/api/v1/users/me/password" && request.method() == Request::kPost;
    const bool validSession = session && static_cast<bool>(SecretToken::session(*session));
    state->clear = web_ && protection && same && ((validSession && (logout || password)) || (!session && auth.status == Single::Missing && logout));
    return BrowserRequest{std::get<AuthenticationSource>(source), session.has_value(), validSession, protection && same, std::move(state)};
}
std::optional<ProtocolError> BrowserSecurity::anonymousMode(CredentialMode mode, const BrowserRequest& request) const {
    if (mode == CredentialMode::Bearer) return request.session ? std::optional<ProtocolError>(ProtocolError::InvalidArgument) : std::nullopt;
    if (!web_) return ProtocolError::CookieModeDisabled;
    if (!request.sameOrigin) return ProtocolError::RequestOriginRejected;
    if (request.session && !request.validSession) return ProtocolError::Unauthenticated;
    return std::nullopt;
}
muduox::net::HttpResponseHeaderPolicy BrowserSecurity::responsePolicy(std::shared_ptr<CookieResponseState> state) {
    return [state](const auto&, auto, auto status, auto& headers) {
        if (state->clear && (!state->login || static_cast<int>(status) >= 400)) headers.set("Set-Cookie", kClearSessionCookie);
    };
}
}  // namespace anof::http
