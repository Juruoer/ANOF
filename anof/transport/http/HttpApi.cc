#include <anof/transport/http/HttpApi.h>
#include <anof/transport/http/BearerCredentials.h>
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
}  // namespace

HttpApi::HttpApi(const SessionTokenCodec& codec, const RequestIdentity& identity,
                 std::function<bool()> ready, CreateDirectoryStream stream,
                 HttpObservation observation, HttpApiOptions options)
    : trace_(identity, std::move(observation)) {
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
    RouterHttpOptions routing;
    routing.methodNotAllowed = true;
    RouterHttpAdapter adapter(router, [](const auto&, auto* response) { response->setStatusCode(HttpResponse::k404NotFound); }, routing);
    auto terminal = [adapter, trace = trace_, &codec, ready = std::move(ready), stream = std::move(stream), options]
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
                    const auto credential = readBearerCredentials(head.headers(), codec);
                    if (const auto* error = std::get_if<ProtocolError>(&credential)) return reject(*error, id);
                    if (const auto error = validateJsonContentHeaders(head.headers())) return reject(*error, id);
                    const RequestContext context{ std::get<SessionDigest>(credential), id };
                    return HttpRequestDecision::streaming([context, stream](const auto& req, const auto& writer) {
                        return stream(context, req, writer);
                    });
                }();
                selected.attributes().merge(original.attributes());
                if (selection->match.endpoint->pattern == "/api/v1/nodes") {
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
