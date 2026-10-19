#include <anof/transport/http/IdentityStream.h>
#include <anof/transport/http/IdentityResponse.h>
#include <anof/transport/http/internal/FlatJson.h>
#include <muduox/net/EventLoop.h>
#include <muduox/net/router/HttpStreamSender.h>

namespace anof::http::internal {
using namespace muduox::net;
namespace {
struct ClearTextOnExit {
    std::string& value;
    ~ClearTextOnExit() { clearText(value); }
};
struct ClearBufferOnExit {
    ExchangeBuffer& value;
    ~ClearBufferOnExit() { value.clear(); }
};
}
AsyncRequestState::AsyncRequestState(RequestId id, RequestBody bodyKind, std::shared_ptr<HttpResponseWriter> writer, std::function<bool()> ready)
    : id_(id), bodyKind_(bodyKind), writer_(std::move(writer)), ready_(std::move(ready)), loop_(EventLoop::getEventLoopOfCurrentThread()), body_(bodyKind == RequestBody::Json ? kMaxJsonBytes : 0) {
    if (!loop_ || !writer_ || !ready_) throw std::invalid_argument("Async request requires owning loop, writer and readiness callback");
}
HttpStreamCallbacks AsyncRequestState::callbacks(Operation operation) {
    operation_ = std::move(operation);
    const auto self = shared_from_this();
    return {[self](std::string_view part) { return self->body(part); },
            [self](const HttpRequest& request) { self->complete(request); },
            [self] { self->cancel(); }};
}
std::size_t AsyncRequestState::body(std::string_view part) {
    loop_->assertInLoopThread();
    if (cancelled_ || inputComplete_) return 0;
    try {
        if (!body_.append(part)) {
            inputComplete_ = true;
            body_.clear();
            fail(bodyKind_ == RequestBody::None ? ProtocolError::InvalidArgument : ProtocolError::RequestTooLarge, true);
        }
    } catch (...) { abort(); }
    return part.size();
}
void AsyncRequestState::complete(const HttpRequest& request) {
    loop_->assertInLoopThread();
    if (cancelled_ || inputComplete_) return;
    inputComplete_ = true;
    ClearBufferOnExit clear{body_};
    try {
        if (!request.trailers().empty()) { fail(ProtocolError::InvalidArgument, true); return; }
        if (!ready_()) { fail(ProtocolError::TemporarilyUnavailable); return; }
        // 将 callable 移到局部，允许 writer 回调同步触发取消而不销毁正在执行的 callable。
        auto operation = std::move(operation_);
        auto submission = operation(body_.view(), weak_from_this());
        if (auto* accepted = std::get_if<AcceptedTask>(&submission)) {
            if (!accepted->handle) { abort(); return; }
            if (cancelled_) { accepted->handle->cancelBeforeStart(); return; }
            task_ = std::move(accepted->handle);
        } else if (const auto* error = std::get_if<SubmitError>(&submission)) fail(protocolError(*error));
        else fail(std::get<ProtocolError>(submission));
    } catch (...) { abort(); }
}
bool AsyncRequestState::beginCompletion(std::uint64_t taskId) {
    loop_->assertInLoopThread();
    if (cancelled_ || taskComplete_ || !writer_->active()) return false;
    if (!task_ || task_->id() != taskId) { abort(); return false; }
    taskComplete_ = true;
    task_.reset();
    return true;
}
void AsyncRequestState::respond(Result<JsonResponse> response) {
    if (!response) fail(ProtocolError::InternalError);
    else send(std::move(response).value());
}
void AsyncRequestState::fail(ProtocolError error, bool close) {
    auto response = errorResponse(error, id_);
    response.headers.setCloseConnection(close);
    send(std::move(response));
}
void AsyncRequestState::send(JsonResponse response) {
    loop_->assertInLoopThread();
    ClearTextOnExit clear{response.body};
    if (cancelled_ || sender_ || !writer_->active()) return;
    if (response.body.size() > kMaxIdentityResponseBytes || !response.headers.body().empty()) { abort(); return; }
    response_ = std::make_shared<ExchangeBuffer>(response.body.size());
    response_->append(response.body);
    const auto length = response.body.size();
    // 在调用 writer（可能重入 cancel）之前清除临时 JSON 正文。
    clearText(response.body);
    sender_ = HttpStreamSender::create(writer_, std::move(response.headers), [buffer = response_]() -> std::optional<std::string> {
        if (buffer->view().empty()) return std::nullopt;
        return buffer->take(HttpResponseWriter::kMaxWriteBytes);
    }, response.omitLength ? std::nullopt : std::optional<std::size_t>(length));
    sender_->send();
}
void AsyncRequestState::cancel() {
    loop_->assertInLoopThread();
    if (cancelled_) return;
    cancelled_ = true;
    body_.clear();
    operation_ = {};
    if (task_) { task_->cancelBeforeStart(); task_.reset(); }
    if (response_) response_->clear();
    if (sender_) sender_->cancel();
}
void AsyncRequestState::abort() {
    cancel();
    writer_->abort();
}
}  // namespace anof::http::internal
