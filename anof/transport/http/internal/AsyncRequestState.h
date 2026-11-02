#pragma once

#include <anof/application/ports/TaskDispatcher.h>
#include <anof/transport/http/JsonResponse.h>
#include <anof/transport/http/internal/ExchangeBuffer.h>
#include <muduox/net/http/HttpResponseWriter.h>

namespace muduox::net {
class EventLoop;
class HttpStreamSender;
}  // namespace muduox::net
namespace anof::http {
enum class RequestBody { None,
                         Json };
namespace internal {
class AsyncRequestState : public std::enable_shared_from_this<AsyncRequestState> {
public:
    using Submission = std::variant<AcceptedTask, SubmitError, ProtocolError>;
    using Operation = std::function<Submission(std::string_view, std::weak_ptr<AsyncRequestState>)>;
    AsyncRequestState(RequestId id, RequestBody body, std::shared_ptr<muduox::net::HttpResponseWriter> writer, std::function<bool()> ready);
    muduox::net::HttpStreamCallbacks callbacks(Operation operation);
    // 完成回调可能晚于 HTTP 终态；通过此检查后才可转换结果（尤其是秘密结果）。
    bool beginCompletion(std::uint64_t taskId);
    void respond(Result<JsonResponse> response);
    void fail(ProtocolError error, bool close = false);
    void abort();

private:
    std::size_t body(std::string_view part);
    void complete(const muduox::net::HttpRequest& request);
    void cancel();
    void send(JsonResponse response);
    RequestId id_;
    RequestBody bodyKind_;
    std::shared_ptr<muduox::net::HttpResponseWriter> writer_;
    std::function<bool()> ready_;
    muduox::net::EventLoop* loop_;
    ExchangeBuffer body_;
    Operation operation_;
    std::unique_ptr<TaskHandle> task_;
    std::shared_ptr<ExchangeBuffer> response_;
    std::shared_ptr<muduox::net::HttpStreamSender> sender_;
    bool inputComplete_ = false;
    bool taskComplete_ = false;
    bool cancelled_ = false;
};
}  // namespace internal
}  // namespace anof::http
