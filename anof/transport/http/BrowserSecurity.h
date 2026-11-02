#pragma once
#include <anof/application/ports/InstancePreparation.h>
#include <anof/transport/http/IdentityRequest.h>
#include <muduox/net/http/HttpRequest.h>
#include <muduox/net/http/HttpResponsePolicy.h>
namespace anof::http {
inline constexpr std::string_view kClearSessionCookie = "__Host-anof_session=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT";
struct CookieResponseState {
    bool clear = false;
    bool login = false;
    CredentialMode loginMode = CredentialMode::Bearer;
};
struct BrowserRequest {
    AuthenticationSource source;
    bool session = false;
    bool validSession = false;
    bool sameOrigin = false;
    std::shared_ptr<CookieResponseState> response;
};
class BrowserSecurity {
public:
    explicit BrowserSecurity(const InstanceConfiguration&);
    Parsed<BrowserRequest> inspect(const muduox::net::HttpRequest&, const muduox::net::HttpRequestMetadata&) const;
    std::optional<ProtocolError> anonymousMode(CredentialMode, const BrowserRequest&) const;
    static muduox::net::HttpResponseHeaderPolicy responsePolicy(std::shared_ptr<CookieResponseState>);

private:
    bool development_, web_;
    std::string origin_, authority_;
    std::vector<std::string> proxies_;
};
}  // namespace anof::http
