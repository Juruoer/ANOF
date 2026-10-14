#include <anof/transport/http/CreateDirectoryHandler.h>
#include <anof/transport/http/CreateDirectoryJson.h>
#include <muduox/net/EventLoop.h>
#include <muduox/net/router/HttpStreamSender.h>

#include <stdexcept>
#include <utility>

namespace anof::http {
namespace {
using namespace muduox::net;

class RequestState : public std::enable_shared_from_this<RequestState> {
public:
    RequestState(RequestContext context, const std::shared_ptr<HttpResponseWriter>& writer,
                 CreateDirectoryDispatcher& dispatcher, std::function<bool()> ready, DirectoryTaskObserver observer)
        : context_(context), writer_(writer), dispatcher_(dispatcher), ready_(std::move(ready)),
          observer_(std::move(observer)), loop_(EventLoop::getEventLoopOfCurrentThread()) {
        if (!loop_) throw std::logic_error("HTTP handler requires an owning loop");
    }
    std::size_t body(std::string_view part) {
        loop_->assertInLoopThread();
        if (cancelled_ || completed_) return 0;
        if (part.size() > kMaxJsonBytes - body_.size()) {
            auto response = errorResponse(ProtocolError::RequestTooLarge, context_.requestId);
            response.headers.setCloseConnection(true);
            send(std::move(response));
            completed_ = true;
            return part.size();
        }
        body_.append(part);
        return part.size();
    }
    void complete(const HttpRequest& request) {
        loop_->assertInLoopThread();
        if (cancelled_ || completed_) return;
        completed_ = true;
        if (!request.trailers().empty()) { fail(ProtocolError::InvalidArgument); return; }
        auto parsed = parseCreateDirectoryJson(body_);
        std::string().swap(body_);
        if (const auto* error = std::get_if<ProtocolError>(&parsed)) { fail(*error); return; }
        if (!ready_()) { fail(ProtocolError::TemporarilyUnavailable); return; }
        const std::weak_ptr<RequestState> weak = shared_from_this();
        auto submission = dispatcher_.trySubmit(context_, std::get<CreateDirectoryCommand>(std::move(parsed)),
            [weak, id = context_.requestId, observer = observer_](CreateDirectoryCompletion completion) {
                const auto state = weak.lock();
                const bool active = state && !state->cancelled_;
                // 业务终态与 HTTP 终态分离。请求已经释放时仍消费并记录迟到结果。
                if (observer) {
                    const auto* result = std::get_if<Result<CreateDirectoryResult>>(&completion.outcome);
                    std::optional<ErrorCode> error;
                    if (result && !*result) error = result->error().code;
                    try {
                        observer({ id, completion.taskId, completion.attempt, !result, error, active });
                    } catch (...) {
                        // 可丢弃观测不改变事务结果，也不能妨碍 HTTP 发送和完成名额归还。
                    }
                }
                if (active) state->finished(std::move(completion));
            });
        if (auto* accepted = std::get_if<AcceptedTask>(&submission)) {
            task_ = std::move(accepted->handle);
        } else {
            fail(protocolError(std::get<SubmitError>(submission)));
        }
    }
    void cancel() {
        loop_->assertInLoopThread();
        if (cancelled_) return;
        cancelled_ = true;
        std::string().swap(body_);
        if (task_) {
            // 不论取消结果如何，runtime 都仍须交付并消费一次终态；这里不释放任务容量。
            task_->cancelBeforeStart();
            task_.reset();
        }
        if (sender_) sender_->cancel();
    }

private:
    void fail(ProtocolError error) { send(errorResponse(error, context_.requestId)); }
    void send(JsonResponse response) {
        loop_->assertInLoopThread();
        if (cancelled_ || sender_ || !writer_->active()) return;
        const auto length = response.body.size();
        sender_ = HttpStreamSender::create(writer_, std::move(response.headers),
            [body = std::move(response.body)]() mutable -> std::optional<std::string> {
                if (body.empty()) return std::nullopt;
                return std::exchange(body, {});
            }, length);
        sender_->send();
    }
    void finished(CreateDirectoryCompletion completion) {
        loop_->assertInLoopThread();
        task_.reset();
        try {
            const auto* result = std::get_if<Result<CreateDirectoryResult>>(&completion.outcome);
            if (!result) { fail(ProtocolError::TemporarilyUnavailable); return; }
            if (!*result) { fail(protocolError(result->error().code)); return; }
            auto response = createdDirectoryResponse(result->value(), context_.requestId);
            if (!response) { fail(ProtocolError::InternalError); return; }
            send(std::move(response).value());
        } catch (...) {
            // 完成回调不再处于 HTTP 调用栈。异常必须显式终止交换，不能等待框架捕获，
            // 也不能在 sender 可能已提交头部后追加另一份错误响应。
            writer_->abort();
        }
    }
    RequestContext context_;
    std::shared_ptr<HttpResponseWriter> writer_;
    CreateDirectoryDispatcher& dispatcher_;
    std::function<bool()> ready_;
    DirectoryTaskObserver observer_;
    EventLoop* loop_;
    std::string body_;
    std::unique_ptr<TaskHandle> task_;
    std::shared_ptr<HttpStreamSender> sender_;
    bool completed_ = false;
    bool cancelled_ = false;
};
}  // namespace

CreateDirectoryHandler::CreateDirectoryHandler(CreateDirectoryDispatcher& dispatcher, std::function<bool()> ready, DirectoryTaskObserver observer)
    : dispatcher_(&dispatcher), ready_(std::move(ready)), observer_(std::move(observer)) {
    if (!ready_) throw std::invalid_argument("CreateDirectory handler requires readiness callback");
}

CreateDirectoryStream CreateDirectoryHandler::stream() const {
    return [dispatcher = dispatcher_, ready = ready_, observer = observer_](RequestContext context, const auto&, const auto& writer) {
        auto state = std::make_shared<RequestState>(context, writer, *dispatcher, ready, observer);
        HttpStreamCallbacks callbacks;
        callbacks.body = [state](std::string_view part) { return state->body(part); };
        callbacks.complete = [state](const HttpRequest& request) { state->complete(request); };
        callbacks.cancel = [state] { state->cancel(); };
        return callbacks;
    };
}
}  // namespace anof::http
