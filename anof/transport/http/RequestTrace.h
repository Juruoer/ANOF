#pragma once

#include <anof/application/ports/RequestIdentity.h>
#include <muduox/net/http/HttpResponsePolicy.h>

namespace anof::http {

// 不携带 rawTarget、query、头部、正文或摘要，避免观测端意外记录敏感值。
struct HttpTraceRecord {
    RequestId requestId;
    muduox::net::HttpExchangeId exchange;
    std::optional<muduox::net::HttpRequest::Method> method;
    std::optional<int> status;
    std::optional<muduox::net::HttpResponseSource> source;
    muduox::net::HttpInputState input;
    std::size_t requestBodyBytes;
    std::size_t responseBodyBytes;
    std::chrono::steady_clock::time_point started;
    std::optional<std::chrono::steady_clock::time_point> committed;
    std::optional<std::chrono::steady_clock::time_point> ended;
    std::optional<muduox::net::HttpExchangeOutcome> outcome;
    std::optional<muduox::net::HttpCancelReason> reason;
    std::string_view route = "unmatched";  // static template, never a request view
};

struct HttpObservation {
    // 所属 loop 同步调用；sink 必须非阻塞且有界。终态记录只来自 finished。
    std::function<void(HttpTraceRecord)> committed;
    std::function<void(HttpTraceRecord)> finished;
};

class RequestTrace {
public:
    RequestTrace(const RequestIdentity& identity, HttpObservation observation = {});
    RequestId requestId(muduox::net::HttpExchangeId exchange) const;
    muduox::net::HttpResponseHeaderPolicy headerPolicy() const;
    muduox::net::HttpExchangeObserverFactory observerFactory() const;

private:
    const RequestIdentity* identity_;
    HttpObservation observation_;
};

}  // namespace anof::http
