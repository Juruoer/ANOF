#pragma once
#include <anof/transport/http/DownloadStream.h>
#include <anof/transport/http/DownloadResponse.h>
namespace anof::http::internal {
struct DownloadStateTestAccess;
// 每交换单独状态；所有入口仅在 writer 的 owner loop 调用。
class DownloadState : public std::enable_shared_from_this<DownloadState> {
public:
    DownloadState(TransferDispatcher&, RequestContext, OpenDownloadCommand, muduox::net::HttpHeaders,
        std::function<bool()>, std::shared_ptr<muduox::net::HttpResponseWriter>);
    muduox::net::HttpStreamCallbacks callbacks();
private:
    friend struct DownloadStateTestAccess;
    enum class Wait { None, Start, Write, Finish };
    void complete(const muduox::net::HttpRequest&);
    void open(TaskCompletion<OpenedDownload>);
    void pull();
    void read(TaskCompletion<OwnedTransferBlock>);
    void pump();
    void fail(ProtocolError);
    void abort();
    void cleanup();
    void releaseStream();
    bool begin(std::uint64_t);
    bool accept(TaskSubmission);
    bool handle(muduox::net::HttpResponseWriter::Result, Wait);
    TransferDispatcher& dispatcher_;
    RequestContext context_;
    OpenDownloadCommand command_;
    muduox::net::HttpHeaders conditions_;
    std::function<bool()> ready_;
    std::shared_ptr<muduox::net::HttpResponseWriter> writer_;
    // 局部 shared 所有权只用于保护 consumed/setCancellationCallback 的同步重入。
    std::shared_ptr<anof::DownloadStream> stream_;
    std::unique_ptr<TaskHandle> task_;
    OwnedTransferBlock buffer_;
    TransferBlock* expectedBuffer_ = nullptr;
    DownloadResponse response_;
    std::string errorBody_;
    std::size_t sent_ = 0, blockOffset_ = 0, requested_ = 0;
    Wait waiting_ = Wait::None;
    bool inputComplete_ = false, terminal_ = false, started_ = false, responding_ = false;
};
}  // namespace anof::http::internal
