#include <anof/transport/http/RequestTrace.h>

#include <utility>

namespace anof::http {
namespace {
std::string_view route(const std::optional<std::string>& target) {
    if (!target) return "unmatched";
    std::string_view path = *target;
    path = path.substr(0, path.find('?'));
    static constexpr std::string_view patterns[] = {
        "/api/v1/admin/invites",
        "/api/v1/admin/invites/:invite_id",
        "/api/v1/admin/spaces/:space_id/contributions/:user_id/quota",
        "/api/v1/admin/spaces/:space_id/quota",
        "/api/v1/admin/storage",
        "/api/v1/admin/users",
        "/api/v1/admin/users/:user_id",
        "/api/v1/auth/register",
        "/api/v1/auth/session",
        "/api/v1/auth/sessions",
        "/api/v1/nodes",
        "/api/v1/nodes/:node_id",
        "/api/v1/nodes/:node_id/children",
        "/api/v1/nodes/:node_id/content",
        "/api/v1/nodes/:node_id/copy",
        "/api/v1/search",
        "/api/v1/spaces",
        "/api/v1/trash",
        "/api/v1/trash/:node_id",
        "/api/v1/trash/:node_id/restore",
        "/api/v1/uploads",
        "/api/v1/uploads/:upload_id",
        "/api/v1/uploads/:upload_id/complete",
        "/api/v1/users/me",
        "/api/v1/users/me/password",
        "/health/live",
        "/health/ready",
    };
    for (auto pattern : patterns) {
        auto input = path, rest = pattern;
        bool matched = true;
        while (!rest.empty() || !input.empty()) {
            const auto x = input.find('/'), y = rest.find('/');
            const auto a = input.substr(0, x), b = rest.substr(0, y);
            if (b.empty() ? !a.empty() : b.front() == ':' ? a.empty()
                                                          : a != b) {
                matched = false;
                break;
            }
            if (x == std::string_view::npos || y == std::string_view::npos) {
                matched = x == y;
                break;
            }
            input.remove_prefix(x + 1);
            rest.remove_prefix(y + 1);
        }
        if (matched) return pattern;
    }
    return "unmatched";
}
HttpTraceRecord record(RequestId id, const muduox::net::HttpExchangeReport& report) {
    std::optional<int> status;
    if (report.status) status = static_cast<int>(*report.status);
    return { id, report.exchange.id, report.method, status, report.source, report.input,
             report.requestBodyBytes, report.responseBodyBytes, report.exchange.started,
             report.committed, report.ended, report.outcome, report.reason, route(report.rawTarget) };
}
}  // namespace

RequestTrace::RequestTrace(const RequestIdentity& identity, HttpObservation observation)
    : identity_(&identity), observation_(std::move(observation)) {}

RequestId RequestTrace::requestId(muduox::net::HttpExchangeId exchange) const {
    return identity_->forExchange(exchange.connection, exchange.request);
}

muduox::net::HttpResponseHeaderPolicy RequestTrace::headerPolicy() const {
    return [trace = *this](const auto& request, auto, auto, auto& headers) {
        headers.set("X-Request-ID", trace.requestId(request.exchange).toString());
        // 下载表示要求 private；其余响应仍强制 no-store。
        if (headers.view().getSingle("Cache-Control").value != "private,no-store") headers.set("Cache-Control", "no-store");
    };
}

muduox::net::HttpExchangeObserverFactory RequestTrace::observerFactory() const {
    return [trace = *this](const muduox::net::HttpExchangeStart& start) {
        const auto id = trace.requestId(start.id);
        muduox::net::HttpExchangeObserver observer;
        if (trace.observation_.committed) {
            observer.committed = [id, sink = trace.observation_.committed](const auto& report) { sink(record(id, report)); };
        }
        if (trace.observation_.finished) {
            observer.finished = [id, sink = trace.observation_.finished](const auto& report) { sink(record(id, report)); };
        }
        return observer;
    };
}
}  // namespace anof::http
