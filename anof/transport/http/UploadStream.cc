#include <anof/transport/http/UploadStream.h>
#include <anof/transport/http/TransferResponse.h>
#include <muduox/net/EventLoop.h>

#include <algorithm>
#include <cstring>
namespace anof::http {
namespace {
using namespace muduox::net;
class PatchState : public std::enable_shared_from_this<PatchState> {
public:
    PatchState(UploadDispatcher& dispatcher, RequestContext context, PreparePatchCommand command, std::shared_ptr<HttpResponseWriter> writer)
        : dispatcher_(dispatcher), context_(context), command_(command), writer_(std::move(writer)), loop_(EventLoop::getEventLoopOfCurrentThread()) {}
    HttpStreamCallbacks callbacks() {
        auto self = shared_from_this();
        loop_->queueInLoop([weak = weak_from_this()] { if (auto state = weak.lock()) state->prepare(); });
        return {[self](std::string_view bytes) { return self->body(bytes); },
            [self](const HttpRequest& request) { self->complete(request); }, [self] { self->cancel(); }};
    }
private:
    bool active() const { return !cancelled_ && !responded_ && writer_->active(); }
    void retireTask() { if (task_) { task_->cancelBeforeStart(); task_.reset(); } }
    void cancel() {
        if (cancelled_) return;
        cancelled_ = true; retireTask();
        if (stream_) { stream_->cancel(); stream_.reset(); }
        buffer_.reset();
        writer_->setWritableCallback({});
        response_.body.clear();
    }
    void abort() { cancel(); writer_->abort(); }
    void send(JsonResponse value) {
        if (!active()) return;
        responded_ = true;
        retireTask();
        if (stream_) { stream_->cancel(); stream_.reset(); }
        buffer_.reset();
        response_ = std::move(value);
        writer_->setWritableCallback([weak = weak_from_this()] { if (auto state = weak.lock()) state->pump(); });
        pump();
    }
    void pump() {
        // 错误响应完成后显式 abort 未消费的输入；abort 排空已提交输出再关闭连接。
        if (cancelled_ || !responded_ || !writer_->active()) return;
        using Status = HttpResponseWriter::Status;
        try {
            const auto accepted = [this](HttpResponseWriter::Result result) {
                if (result.status == Status::Success) return true;
                if (result.status != Status::WouldBlock) abort();
                return false;
            };
            if (!responseStarted_) {
                if (!accepted(writer_->start(response_.headers, response_.body.size()))) return;
                responseStarted_ = true;
            }
            while (responseOffset_ < response_.body.size()) {
                auto result = writer_->write(std::string_view(response_.body).substr(responseOffset_, HttpResponseWriter::kMaxWriteBytes));
                responseOffset_ += result.acceptedBytes;
                if (!accepted(result)) return;
            }
            if (!accepted(writer_->finish())) return;
            writer_->setWritableCallback({});
            response_.body.clear();
            if (!inputComplete_) abort();
        } catch (...) { abort(); }
    }

    void fail(ProtocolError error) {
        auto response = errorResponse(error, context_.requestId);
        // prepare/feed 拒绝时仍可能有未消费输入，不能复用连接。
        response.headers.setCloseConnection(true); send(std::move(response));
    }
    bool accepted(TaskSubmission value) {
        if (auto* task = std::get_if<AcceptedTask>(&value)) {
            if (!task->handle) { abort(); return false; }
            if (!active()) { task->handle->cancelBeforeStart(); return false; }
            task_ = std::move(task->handle); return true;
        }
        fail(protocolError(std::get<SubmitError>(value))); return false;
    }
    bool begin(std::uint64_t id) {
        if (!active()) return false;
        if (!task_ || task_->id() != id) { abort(); return false; }
        task_.reset(); return true;
    }
    void prepare() {
        if (!active()) return;
        try {
            accepted(dispatcher_.submit(context_, command_, [weak = weak_from_this()](TaskCompletion<PatchPreparation> completed) {
                auto state = weak.lock();
                if (!state || !state->begin(completed.taskId)) {
                    if (auto* result = std::get_if<Result<PatchPreparation>>(&completed.outcome); result && *result) {
                        if (auto* patch = std::get_if<PreparedPatch>(&result->value()); patch && patch->stream) patch->stream->cancel();
                    }
                    return;
                }
                try {
                    auto* result = std::get_if<Result<PatchPreparation>>(&completed.outcome);
                    if (!result) { state->fail(ProtocolError::TemporarilyUnavailable); return; }
                    if (!*result) { state->fail(transferError(result->error().code)); return; }
                    if (auto* conflict = std::get_if<PatchOffsetConflict>(&result->value())) {
                        auto response = patchConflictResponse(*conflict, state->context_.requestId); response.headers.setCloseConnection(true); state->send(std::move(response)); return;
                    }
                    auto patch = std::get<PreparedPatch>(std::move(*result).value());
                    if (!patch.stream || patch.offset != state->command_.offset || patch.stream->generation() != patch.generation) {
                        if (patch.stream) patch.stream->cancel();
                        state->abort(); return;
                    }
                    state->stream_ = std::move(patch.stream);
                    state->buffer_ = std::make_unique<TransferBlock>();
                    state->writer_->resumeBody();
                } catch (...) { state->abort(); }
            }));
        } catch (...) { abort(); }
    }
    std::size_t body(std::string_view bytes) {
        if (!active() || !stream_ || !buffer_ || task_ || inputComplete_) return 0;
        const auto remaining = static_cast<std::size_t>(command_.length.value()) - consumed_;
        const auto size = std::min({bytes.size(), kTransferBlockBytes, remaining});
        if (!size) { if (!bytes.empty()) fail(ProtocolError::InvalidArgument); return 0; }
        try {
            buffer_->size = size; std::memcpy(buffer_->bytes.data(), bytes.data(), size);
            auto* original = buffer_.get();
            auto submission = stream_->feed(buffer_, [weak = weak_from_this(), original](TaskCompletion<OwnedTransferBlock> completed) {
                auto state = weak.lock(); if (!state || !state->begin(completed.taskId)) return;
                try {
                    auto* result = std::get_if<Result<OwnedTransferBlock>>(&completed.outcome);
                    if (!result) { state->fail(ProtocolError::TemporarilyUnavailable); return; }
                    if (!*result) { state->fail(transferError(result->error().code)); return; }
                    auto buffer = std::move(*result).value();
                    if (!buffer || buffer.get() != original || buffer->size > kTransferBlockBytes) { state->abort(); return; }
                    buffer->size = 0; state->buffer_ = std::move(buffer);
                    if (state->inputComplete_) state->finish();
                    else state->writer_->resumeBody();
                } catch (...) { state->abort(); }
            });
            if (std::holds_alternative<AcceptedTask>(submission)) {
                if (buffer_) { abort(); return 0; }
                consumed_ += size;
                if (!accepted(std::move(submission))) return 0;
                return size;
            }
            // 拒绝不得夺走缓冲；本次输入尚未消费。
            if (!buffer_) { abort(); return 0; }
            accepted(std::move(submission));
        } catch (...) { abort(); }
        return 0;
    }
    void complete(const HttpRequest& request) {
        if (!active() || inputComplete_) return;
        inputComplete_ = true;
        try {
            if (!request.trailers().empty() || consumed_ != static_cast<std::size_t>(command_.length.value())) { fail(ProtocolError::InvalidArgument); return; }
            if (!task_) finish();
        } catch (...) { abort(); }
    }
    void finish() {
        if (!active() || !stream_ || task_ || !inputComplete_ || finishing_) return;
        finishing_ = true;
        try {
            accepted(stream_->finish([weak = weak_from_this()](TaskCompletion<UploadOffset> completed) {
                auto state = weak.lock(); if (!state || !state->begin(completed.taskId)) return;
                try {
                    auto* result = std::get_if<Result<UploadOffset>>(&completed.outcome);
                    if (!result) { state->fail(ProtocolError::TemporarilyUnavailable); return; }
                    if (!*result) { state->fail(transferError(result->error().code)); return; }
                    const auto offset = result->value().value();
                    if (offset < state->command_.offset.value() || offset - state->command_.offset.value() != state->command_.length.value()) { state->abort(); return; }
                    state->send(patchResponse(result->value(), state->context_.requestId));
                } catch (...) { state->abort(); }
            }));
        } catch (...) { abort(); }
    }
    UploadDispatcher& dispatcher_;
    RequestContext context_;
    PreparePatchCommand command_;
    std::shared_ptr<HttpResponseWriter> writer_;
    EventLoop* loop_;
    std::unique_ptr<PatchStream> stream_;
    OwnedTransferBlock buffer_;
    std::unique_ptr<TaskHandle> task_;
    JsonResponse response_;
    std::size_t responseOffset_ = 0;
    bool responseStarted_ = false;
    std::size_t consumed_ = 0;
    bool inputComplete_ = false, finishing_ = false, cancelled_ = false, responded_ = false;
};
}
HttpStreamCallback makeUploadPatchStream(UploadDispatcher& dispatcher, RequestContext context, PreparePatchCommand command) {
    return [&dispatcher, context, command](const auto&, const auto& writer) {
        return std::make_shared<PatchState>(dispatcher, context, command, writer)->callbacks();
    };
}
}  // namespace anof::http
