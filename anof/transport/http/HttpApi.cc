#include <anof/transport/http/HttpApi.h>
#include <anof/transport/http/BearerCredentials.h>
#include <anof/transport/http/IdentityStream.h>
#include <anof/transport/http/IdentityResponse.h>
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/AccountRequest.h>
#include <anof/transport/http/AccountResponse.h>
#include <muduox/net/EventLoop.h>
#include <muduox/net/middleware/AdmissionLimits.h>
#include <muduox/net/router/RouterHttpAdapter.h>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace anof::http {
using namespace muduox::net;
namespace {
void registered(const Router::RegisterResult& result) {
    if (result.code != Router::RegisterCode::Success) throw std::logic_error("HTTP route registration failed");
}
HttpRequestDecision reject(ProtocolError error, RequestId id) {
    return HttpRequestDecision::respond(errorResponse(error, id).buffered(true), HttpRequestDecision::BodyDisposition::Close);
}
template <typename Command, typename Value, typename Formatter>
HttpRequestDecision bodylessDecision(Parsed<Command> parsed, IdentityDispatcher& dispatcher,
    RequestContext context, Formatter formatter, const std::function<bool()>& ready) {
    if (const auto* error = std::get_if<ProtocolError>(&parsed)) return reject(*error, context.requestId);
    // 选择阶段结束后只保留自有类型化命令，不借用 route params 或 query。
    return HttpRequestDecision::streaming(makeIdentityStream<Command, Value>(dispatcher, context, RequestBody::None,
        [command = std::get<Command>(std::move(parsed))](std::string_view) -> Parsed<Command> { return command; }, formatter, ready));
}
}  // namespace

HttpApi::HttpApi(const SessionTokenCodec& codec, const RequestIdentity& identity,
                 std::function<bool()> ready, CreateDirectoryStream stream, IdentityDispatcher& identityDispatcher,
                 HttpObservation observation, HttpApiOptions options)
    : identityDispatcher_(identityDispatcher), trace_(identity, std::move(observation)) {
    if (!ready || !stream || options.maxInFlight == 0 || options.maxInFlight > 128 ||
        !std::isfinite(options.bodySeconds) || options.bodySeconds <= 0 || options.bodySeconds > 15 ||
        !std::isfinite(options.responseSeconds) || options.responseSeconds <= 0 || options.responseSeconds > 30) {
        throw std::invalid_argument("Invalid HTTP API configuration");
    }
    auto router = std::make_shared<Router>();
    // 端点策略将 metadata 绑定到实际 callable；注册占位 callable 不应被执行。
    const auto unbound = [](const HttpRequest&, const Router::RouteParams&, HttpResponse*) { throw std::logic_error("Unbound HTTP endpoint"); };
    registered(router->addRoute(HttpRequest::kGet, "/health/live", unbound, 0));
    registered(router->addRoute(HttpRequest::kGet, "/health/ready", unbound, 0));
    registered(router->addStreamRoute(HttpRequest::kPost, "/api/v1/nodes",
        [](const HttpRequest&, const Router::RouteParams&, const auto&) -> HttpStreamCallbacks { throw std::logic_error("Unbound HTTP stream"); }, kMaxJsonBytes));
    for (const auto* path : {"/api/v1/users/me", "/api/v1/spaces", "/api/v1/nodes/:node_id", "/api/v1/nodes/:node_id/children",
                             "/api/v1/admin/users", "/api/v1/admin/users/:user_id", "/api/v1/admin/spaces/:space_id/quota",
                             "/api/v1/admin/spaces/:space_id/contributions/:user_id/quota"}) {
        registered(router->addStreamRoute(HttpRequest::kGet, path,
            [](const HttpRequest&, const Router::RouteParams&, const auto&) -> HttpStreamCallbacks { throw std::logic_error("Unbound HTTP read stream"); }, 0));
    }
    for (const auto* path : {"/api/v1/auth/register", "/api/v1/auth/sessions", "/api/v1/admin/invites"}) {
        registered(router->addStreamRoute(HttpRequest::kPost, path,
            [](const HttpRequest&, const Router::RouteParams&, const auto&) -> HttpStreamCallbacks { throw std::logic_error("Unbound HTTP identity stream"); }, kMaxJsonBytes));
    }
    registered(router->addStreamRoute(HttpRequest::kGet, "/api/v1/admin/invites",
        [](const HttpRequest&, const Router::RouteParams&, const auto&) -> HttpStreamCallbacks { throw std::logic_error("Unbound HTTP invites stream"); }, 0));
    for (const auto* path : {"/api/v1/auth/session", "/api/v1/admin/invites/:invite_id"}) {
        registered(router->addStreamRoute(HttpRequest::kDelete, path,
            [](const HttpRequest&, const Router::RouteParams&, const auto&) -> HttpStreamCallbacks { throw std::logic_error("Unbound HTTP delete stream"); }, 0));
    }
    for (const auto& route : {std::pair{HttpRequest::kPatch, "/api/v1/users/me"},
                             std::pair{HttpRequest::kPost, "/api/v1/users/me/password"},
                             std::pair{HttpRequest::kPatch, "/api/v1/admin/users/:user_id"},
                             std::pair{HttpRequest::kPut, "/api/v1/admin/spaces/:space_id/quota"},
                             std::pair{HttpRequest::kPut, "/api/v1/admin/spaces/:space_id/contributions/:user_id/quota"}}) {
        registered(router->addStreamRoute(route.first, route.second,
            [](const HttpRequest&, const Router::RouteParams&, const auto&) -> HttpStreamCallbacks { throw std::logic_error("Unbound HTTP account stream"); }, kMaxJsonBytes));
    }
    RouterHttpOptions routing;
    routing.methodNotAllowed = true;
    RouterHttpAdapter adapter(router, [](const auto&, auto* response) { response->setStatusCode(HttpResponse::k404NotFound); }, routing);
    auto terminal = [adapter, trace = trace_, &codec, dispatcher = &identityDispatcher_, ready = std::move(ready), stream = std::move(stream), options]
        (const HttpRequest& request, const HttpRequestMetadata& metadata) {
        const auto id = trace.requestId(metadata.exchange);
        // onMiss 无 metadata；在本次选择中把 ID 绑定到 404，保留 Router 的 405 分支。
        bool missed = false;
        auto decision = adapter.select(request, metadata,
            [&](const HttpRequest& head, const HttpRequestMetadata&, RouterEndpointSelectionHandle selection, HttpRequestDecision original) {
                HttpRequestDecision selected = [&] {
                    if (selection->match.endpoint->pattern == "/health/live") {
                        return HttpRequestDecision::buffered([id](const auto&, auto* response) { *response = liveResponse(id).buffered(false); });
                    }
                    if (selection->match.endpoint->pattern == "/health/ready") {
                        return HttpRequestDecision::buffered([id, ready](const auto&, auto* response) { *response = healthResponse(ready(), id).buffered(false); });
                    }
                    if (!ready()) return reject(ProtocolError::TemporarilyUnavailable, id);
                    const auto& pattern = selection->match.endpoint->pattern;
                    if (pattern == "/api/v1/auth/register" || pattern == "/api/v1/auth/sessions") {
                        if (const auto error = validateAnonymousCredentials(head.headers())) return reject(*error, id);
                        if (const auto error = validateNoQuery(head.query())) return reject(*error, id);
                        if (const auto error = validateJsonContentHeaders(head.headers())) return reject(*error, id);
                        const auto source = readAuthenticationSource(metadata);
                        if (const auto* error = std::get_if<ProtocolError>(&source)) return reject(*error, id);
                        const auto ownedSource = std::get<AuthenticationSource>(source);
                        if (pattern == "/api/v1/auth/register") {
                            return HttpRequestDecision::streaming(makeIdentityStream<RegisterCommand, UserProfile>(*dispatcher, id, RequestBody::Json,
                                [ownedSource](std::string_view body) { return parseRegisterJson(body, ownedSource); }, registeredUserResponse, ready));
                        }
                        return HttpRequestDecision::streaming(makeIdentityStream<LoginCommand, LoginResult>(*dispatcher, id, RequestBody::Json,
                            [ownedSource](std::string_view body) { return parseLoginJson(body, ownedSource); }, loginResponse, ready));
                    }
                    const auto credential = readBearerCredentials(head.headers(), codec);
                    if (const auto* error = std::get_if<ProtocolError>(&credential)) return reject(*error, id);
                    const RequestContext context{{std::get<SessionDigest>(credential), CredentialMode::Bearer}, id};
                    if (pattern == "/api/v1/nodes") {
                        if (const auto error = validateJsonContentHeaders(head.headers())) return reject(*error, id);
                        return HttpRequestDecision::streaming([context, stream](const auto& req, const auto& writer) {
                            return stream(context, req, writer);
                        });
                    }
                    if (pattern == "/api/v1/admin/invites" && head.method() == HttpRequest::kPost) {
                        if (const auto error = validateNoQuery(head.query())) return reject(*error, id);
                        if (const auto error = validateJsonContentHeaders(head.headers())) return reject(*error, id);
                        return HttpRequestDecision::streaming(makeIdentityStream<CreateInviteCommand, CreatedInvite>(*dispatcher, context, RequestBody::Json,
                            parseCreateInviteJson, createdInviteResponse, ready));
                    }
                    const bool quotaWrite = head.method() == HttpRequest::kPut &&
                        (pattern == "/api/v1/admin/spaces/:space_id/quota" || pattern == "/api/v1/admin/spaces/:space_id/contributions/:user_id/quota");
                    if (head.method() == HttpRequest::kPatch || pattern == "/api/v1/users/me/password" || quotaWrite) {
                        if (const auto error = validateNoQuery(head.query())) return reject(*error, id);
                        if (const auto error = validateJsonContentHeaders(head.headers())) return reject(*error, id);
                        auto condition = parseAccountCondition(head.headers());
                        if (const auto* error = std::get_if<ProtocolError>(&condition)) return reject(*error, id);
                        const auto expected = std::get<AccountCondition>(std::move(condition));
                        if (pattern == "/api/v1/users/me") {
                            return HttpRequestDecision::streaming(makeIdentityStream<UpdateDisplayNameCommand, UserProfile>(*dispatcher, context, RequestBody::Json,
                                [expected](std::string_view body) { return parseUpdateDisplayNameJson(body, expected); }, currentUserResponse, ready));
                        }
                        if (pattern == "/api/v1/users/me/password") {
                            auto source = readAuthenticationSource(metadata);
                            if (const auto* error = std::get_if<ProtocolError>(&source)) return reject(*error, id);
                            return HttpRequestDecision::streaming(makeIdentityStream<ChangePasswordCommand, void>(*dispatcher, context, RequestBody::Json,
                                [expected, ownedSource = std::get<AuthenticationSource>(source)](std::string_view body) { return parseChangePasswordJson(body, expected, ownedSource); }, noContentResponse, ready));
                        }
                        if (pattern == "/api/v1/admin/users/:user_id") {
                            const auto userId = Router::findParam(selection->match.params, "user_id");
                            if (!userId) return reject(ProtocolError::InternalError, id);
                            auto user = parseGetUser(*userId);
                            if (const auto* error = std::get_if<ProtocolError>(&user)) return reject(*error, id);
                            return HttpRequestDecision::streaming(makeIdentityStream<SetUserEnabledCommand, AdminUserView>(*dispatcher, context, RequestBody::Json,
                                [expected, target = std::get<GetUserCommand>(user).id](std::string_view body) { return parseSetUserEnabledJson(body, target, expected); }, adminUserResponse, ready));
                        }
                        const auto spaceId = Router::findParam(selection->match.params, "space_id");
                        const auto userId = Router::findParam(selection->match.params, "user_id");
                        if (!spaceId || (pattern == "/api/v1/admin/spaces/:space_id/contributions/:user_id/quota" && !userId)) return reject(ProtocolError::InternalError, id);
                        auto scope = parseQuotaScope(*spaceId, userId);
                        if (const auto* error = std::get_if<ProtocolError>(&scope)) return reject(*error, id);
                        return HttpRequestDecision::streaming(makeIdentityStream<SetQuotaLimitCommand, ManagedQuota>(*dispatcher, context, RequestBody::Json,
                            [expected, target = std::get<QuotaScope>(scope)](std::string_view body) { return parseSetQuotaLimitJson(body, target, expected); }, managedQuotaResponse, ready, quotaUpdateError));
                    }
                    if (const auto error = validateBodylessHeaders(head.headers())) return reject(*error, id);
                    if (pattern != "/api/v1/nodes/:node_id/children" && pattern != "/api/v1/admin/invites" && pattern != "/api/v1/admin/users") {
                        if (const auto error = validateNoQuery(head.query())) return reject(*error, id);
                    }
                    if (pattern == "/api/v1/users/me") {
                        return bodylessDecision<GetMeCommand, UserProfile>(GetMeCommand{}, *dispatcher, context, currentUserResponse, ready);
                    }
                    if (pattern == "/api/v1/spaces") {
                        return bodylessDecision<ListSpacesCommand, std::vector<SpaceView>>(ListSpacesCommand{}, *dispatcher, context, spacesResponse, ready);
                    }
                    if (pattern == "/api/v1/auth/session") {
                        return bodylessDecision<LogoutCommand, void>(LogoutCommand{}, *dispatcher, context, noContentResponse, ready);
                    }
                    if (pattern == "/api/v1/admin/invites") {
                        return bodylessDecision<ListInvitesCommand, Page<InviteInfo, InviteCursor>>(parseListInvites(head.query()), *dispatcher, context, invitesResponse, ready);
                    }
                    if (pattern == "/api/v1/admin/invites/:invite_id") {
                        const auto inviteId = Router::findParam(selection->match.params, "invite_id");
                        if (!inviteId) return reject(ProtocolError::InternalError, id);
                        return bodylessDecision<RevokeInviteCommand, void>(parseRevokeInvite(*inviteId), *dispatcher, context, noContentResponse, ready);
                    }
                    if (pattern == "/api/v1/admin/users") {
                        return bodylessDecision<ListUsersCommand, Page<UserProfile, UserCursor>>(parseListUsers(head.query()), *dispatcher, context, usersResponse, ready);
                    }
                    if (pattern == "/api/v1/admin/users/:user_id") {
                        const auto userId = Router::findParam(selection->match.params, "user_id");
                        if (!userId) return reject(ProtocolError::InternalError, id);
                        return bodylessDecision<GetUserCommand, AdminUserView>(parseGetUser(*userId), *dispatcher, context, adminUserResponse, ready);
                    }
                    if (pattern == "/api/v1/admin/spaces/:space_id/quota" || pattern == "/api/v1/admin/spaces/:space_id/contributions/:user_id/quota") {
                        const auto spaceId = Router::findParam(selection->match.params, "space_id");
                        const auto userId = Router::findParam(selection->match.params, "user_id");
                        if (!spaceId || (pattern == "/api/v1/admin/spaces/:space_id/contributions/:user_id/quota" && !userId)) return reject(ProtocolError::InternalError, id);
                        auto scope = parseQuotaScope(*spaceId, userId);
                        if (const auto* error = std::get_if<ProtocolError>(&scope)) return reject(*error, id);
                        return bodylessDecision<GetQuotaCommand, ManagedQuota>(GetQuotaCommand{std::get<QuotaScope>(std::move(scope))}, *dispatcher, context, managedQuotaResponse, ready);
                    }
                    const auto nodeId = Router::findParam(selection->match.params, "node_id");
                    if (!nodeId) return reject(ProtocolError::InternalError, id);
                    if (pattern == "/api/v1/nodes/:node_id") {
                        return bodylessDecision<GetNodeCommand, NodeDetails>(parseGetNode(*nodeId), *dispatcher, context, nodeResponse, ready);
                    }
                    if (pattern == "/api/v1/nodes/:node_id/children") {
                        return bodylessDecision<ListChildrenCommand, Page<NodeDetails, ChildrenCursor>>(parseListChildren(*nodeId, head.query()), *dispatcher, context, childrenResponse, ready);
                    }
                    return reject(ProtocolError::InternalError, id);
                }();
                selected.attributes().merge(original.attributes());
                if (selection->match.endpoint->pattern != "/health/live" && selection->match.endpoint->pattern != "/health/ready") {
                    selected.attributes().restrictBodyTimeout(options.bodySeconds).restrictResponseTimeout(options.responseSeconds);
                }
                return selected;
            }, [&](const auto&, const auto&, const RouterSelectionResult& selection) { missed = selection.kind == RouterDispatchKind::OnMiss; });
        // 仅替换真正未命中，不覆盖 Router 的 405/Allow。
        if (missed) {
            auto missing = reject(ProtocolError::NotFound, id);
            missing.attributes().merge(decision.attributes());
            return missing;
        }
        return decision;
    };
    InFlightLimitOptions limit;
    limit.perKey = options.maxInFlight;
    limit.total = options.maxInFlight;
    limit.admission.maxKeys = 1;
    limit.admission.rejected = [trace = trace_](const auto&, const auto& metadata, const auto&) {
        return errorResponse(ProtocolError::TemporarilyUnavailable, trace.requestId(metadata.exchange)).buffered(true);
    };
    InFlightLimiter limiter(limit);
    selector_ = compose(std::move(terminal), { limiter.middleware() });
}

void HttpApi::configure(HttpServer& server) {
    server.getLoop()->assertInLoopThread();
    if (configured_) throw std::logic_error("HTTP API already configured");
    server.setThreadNum(0);
    server.setExchangeObserverFactory(trace_.observerFactory());
    server.setResponseHeaderPolicy(trace_.headerPolicy());
    server.setHttpRequestSelector(selector_);
    configured_ = true;
}
}  // namespace anof::http
