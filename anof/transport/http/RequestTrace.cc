#include <anof/transport/http/RequestTrace.h>

#include <utility>

namespace anof::http {
namespace {
HttpTraceRecord record(RequestId id, const muduox::net::HttpExchangeReport& report) {
    std::optional<int> status;
    if (report.status) status = static_cast<int>(*report.status);
    return { id, report.exchange.id, report.method, status, report.source, report.input,
             report.requestBodyBytes, report.responseBodyBytes, report.exchange.started,
             report.committed, report.ended, report.outcome, report.reason };
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
