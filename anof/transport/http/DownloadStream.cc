#include <anof/transport/http/DownloadStream.h>
#include <anof/transport/http/internal/DownloadState.h>
#include <anof/transport/http/TransferResponse.h>
#include <algorithm>
namespace anof::http::internal {
using namespace muduox::net;
DownloadState::DownloadState(TransferDispatcher& dispatcher, RequestContext context, OpenDownloadCommand command,
    HttpHeaders conditions, std::function<bool()> ready, std::shared_ptr<HttpResponseWriter> writer)
    : dispatcher_(dispatcher), context_(context), command_(command), conditions_(std::move(conditions)), ready_(std::move(ready)), writer_(std::move(writer)) {}
HttpStreamCallbacks DownloadState::callbacks() {
    auto self = shared_from_this();
    return {[self](std::string_view bytes) { if (!bytes.empty()) self->abort(); return bytes.size(); },
        [self](const HttpRequest& request) { self->complete(request); }, [self] { self->abort(); }};
}
void DownloadState::releaseStream() {
    auto stream = std::move(stream_);
    if (!stream) return;
    try { stream->setCancellationCallback({}); } catch (...) { /* cancel 仍须发生。 */ }
    stream->cancel();
}
void DownloadState::cleanup() {
    if (task_) { task_->cancelBeforeStart(); task_.reset(); }
    releaseStream(); buffer_.reset(); errorBody_.clear();
    writer_->setWritableCallback({}); waiting_ = Wait::None;
}
void DownloadState::abort() {
    if (terminal_) return;
    terminal_ = true; cleanup(); writer_->abort();
}
bool DownloadState::begin(std::uint64_t id) {
    if (terminal_ || !writer_->active()) return false;
    if (!task_ || task_->id() != id) { abort(); return false; }
    task_.reset(); return true;
}
bool DownloadState::accept(TaskSubmission submission) {
    if (auto* accepted = std::get_if<AcceptedTask>(&submission)) {
        if (!accepted->handle) { abort(); return false; }
        if (terminal_) { accepted->handle->cancelBeforeStart(); return false; }
        task_ = std::move(accepted->handle); return true;
    }
    fail(protocolError(std::get<SubmitError>(submission))); return false;
}
void DownloadState::complete(const HttpRequest& request) {
    if (terminal_ || inputComplete_) return;
    inputComplete_ = true;
    try {
        if (!request.trailers().empty()) { fail(ProtocolError::InvalidArgument); return; }
        if (!ready_()) { fail(ProtocolError::TemporarilyUnavailable); return; }
        accept(dispatcher_.submit(context_, command_, [weak = weak_from_this()](TaskCompletion<OpenedDownload> result) {
            if (auto state = weak.lock()) state->open(std::move(result));
            else if (auto* value = std::get_if<Result<OpenedDownload>>(&result.outcome); value && *value && value->value().stream) value->value().stream->cancel();
        }));
    } catch (...) { abort(); }
}
void DownloadState::open(TaskCompletion<OpenedDownload> completed) {
    if (!begin(completed.taskId)) {
        if (auto* value = std::get_if<Result<OpenedDownload>>(&completed.outcome); value && *value && value->value().stream) value->value().stream->cancel();
        return;
    }
    try {
        auto* result = std::get_if<Result<OpenedDownload>>(&completed.outcome);
        if (!result) { fail(ProtocolError::TemporarilyUnavailable); return; }
        if (!*result) { fail(transferError(result->error().code)); return; }
        auto opened = std::move(*result).value();
        stream_ = std::move(opened.stream);
        if (opened.descriptor.nodeId != command_.id || command_.metadataOnly == static_cast<bool>(stream_)) { fail(ProtocolError::InternalError); return; }
        if (auto stream = stream_) {
            stream->setCancellationCallback([weak = weak_from_this()] { if (auto state = weak.lock()) state->abort(); });
            if (terminal_) return;
        }
        auto plan = downloadResponse(opened.descriptor, conditions_, command_.metadataOnly, context_.requestId);
        conditions_.clear();
        if (!plan) { fail(transferError(plan.error().code)); return; }
        response_ = std::move(plan).value(); responding_ = true;
        if (!response_.content) releaseStream();
        writer_->setWritableCallback([weak = weak_from_this()] {
            if (auto state = weak.lock(); state && state->waiting_ != Wait::None) { state->waiting_ = Wait::None; state->pump(); }
        });
        pump();
    } catch (...) { abort(); }
}
void DownloadState::fail(ProtocolError error) {
    if (terminal_) return;
    if (started_ || responding_) { abort(); return; }
    releaseStream();
    auto value = errorResponse(error, context_.requestId);
    response_.headers = std::move(value.headers); response_.length = value.body.size(); response_.content = false;
    errorBody_ = std::move(value.body); responding_ = true;
    writer_->setWritableCallback([weak = weak_from_this()] {
        if (auto state = weak.lock(); state && state->waiting_ != Wait::None) { state->waiting_ = Wait::None; state->pump(); }
    });
    pump();
}
bool DownloadState::handle(HttpResponseWriter::Result result, Wait stage) {
    if (terminal_) return false;
    if (result.status == HttpResponseWriter::Status::Success) return true;
    if (result.status == HttpResponseWriter::Status::WouldBlock) waiting_ = stage;
    else abort();
    return false;
}
void DownloadState::pump() {
    if (terminal_ || !responding_ || waiting_ != Wait::None || !writer_->active()) return;
    try {
        if (!started_) {
            if (!handle(writer_->start(response_.headers, response_.length), Wait::Start)) return;
            started_ = true;
        }
        if (response_.content) {
            if (task_) return;
            if (buffer_ && buffer_->size > blockOffset_) {
                const auto stream = stream_;
                const auto result = writer_->write(std::string_view(reinterpret_cast<const char*>(buffer_->bytes.data()) + blockOffset_, buffer_->size - blockOffset_));
                blockOffset_ += result.acceptedBytes; sent_ += result.acceptedBytes;
                if (result.acceptedBytes) stream->consumed(result.acceptedBytes);
                if (!handle(result, Wait::Write)) return;
                if (blockOffset_ != buffer_->size) { pump(); return; }
                buffer_->size = 0; blockOffset_ = 0;
            }
            if (sent_ < *response_.length) { pull(); return; }
        } else if (!command_.metadataOnly && sent_ < errorBody_.size()) {
            const auto result = writer_->write(std::string_view(errorBody_).substr(sent_, HttpResponseWriter::kMaxWriteBytes));
            sent_ += result.acceptedBytes;
            if (!handle(result, Wait::Write)) return;
            if (sent_ < errorBody_.size()) { pump(); return; }
        }
        if (!handle(writer_->finish(), Wait::Finish)) return;
        terminal_ = true; cleanup();
    } catch (...) { abort(); }
}
void DownloadState::pull() {
    if (terminal_ || task_ || !stream_) return;
    if (!buffer_) buffer_ = std::make_unique<TransferBlock>();
    requested_ = std::min(kTransferBlockBytes, *response_.length - sent_);
    expectedBuffer_ = buffer_.get();
    auto stream = stream_;
    auto submission = stream->pull(UploadOffset::from(static_cast<std::int64_t>(response_.offset + sent_)).value(), requested_, buffer_,
        [weak = weak_from_this()](TaskCompletion<OwnedTransferBlock> result) { if (auto state = weak.lock()) state->read(std::move(result)); });
    if (std::holds_alternative<AcceptedTask>(submission) ? static_cast<bool>(buffer_) : !buffer_) { abort(); return; }
    accept(std::move(submission));
}
void DownloadState::read(TaskCompletion<OwnedTransferBlock> completed) {
    if (!begin(completed.taskId)) return;
    try {
        auto* result = std::get_if<Result<OwnedTransferBlock>>(&completed.outcome);
        if (!result || !*result) { abort(); return; }
        auto block = std::move(*result).value();
        if (!block || block.get() != expectedBuffer_ || block->size != requested_ || block->size > kTransferBlockBytes) { abort(); return; }
        buffer_ = std::move(block); blockOffset_ = 0; pump();
    } catch (...) { abort(); }
}
}  // namespace anof::http::internal
namespace anof::http {
muduox::net::HttpStreamCallback makeDownloadStream(TransferDispatcher& dispatcher, RequestContext context, OpenDownloadCommand command,
    muduox::net::HttpHeaders conditions, std::function<bool()> ready) {
    return [&dispatcher, context, command, conditions = std::move(conditions), ready = std::move(ready)](const auto&, const auto& writer) {
        return std::make_shared<internal::DownloadState>(dispatcher, context, command, conditions, ready, writer)->callbacks();
    };
}
}  // namespace anof::http
