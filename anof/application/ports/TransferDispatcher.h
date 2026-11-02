#pragma once
#include <anof/application/ports/TaskDispatcher.h>
#include <anof/application/ports/TransferFileStore.h>
#include <anof/application/transfer/TransferCommands.h>

namespace anof {
// 只在 owner loop 调用；每流最多一个自有缓冲、一个在途块。
// feed/pull 接纳时移动 buffer，拒绝时保留调用者所有权且不回调。
// 接纳后恰一次延后完成（含取消），TaskHandle 沿用 queued/running/terminal 取消语义。
// HTTP cancel/销毁流只抑制继续生产：协调器持有独占资格直到磁盘/SQL/尾部清理退休。
using OwnedTransferBlock = std::unique_ptr<TransferBlock>;
class PatchStream {
public:
    PatchStream() = default;
    PatchStream(const PatchStream&) = delete;
    PatchStream& operator=(const PatchStream&) = delete;
    virtual ~PatchStream() = default;  // 实现必须请求取消，不能遗失未退休工作。
    virtual UploadGeneration generation() const noexcept = 0;
    virtual TaskSubmission feed(OwnedTransferBlock& buffer, Completion<OwnedTransferBlock>) = 0;
    // 仅在所有 feed 完成、总数等于声明长度后接纳；fdatasync + 原身份复验 + 提交后成功。
    // 返回确认 offset；未知提交不截断。一次接纳后禁止第二次 finish/feed。
    virtual TaskSubmission finish(Completion<UploadOffset>) = 0;
    virtual void cancel() noexcept = 0;
};
struct PreparedPatch {
    UploadOffset offset;
    UploadGeneration generation;
    std::unique_ptr<PatchStream> stream;
};
// 只有已授权的偏移冲突返回 offset；不能放进未授权通用错误 envelope。
struct PatchOffsetConflict {
    UploadOffset offset;
};
using PatchPreparation = std::variant<PreparedPatch, PatchOffsetConflict>;
class DownloadStream {
public:
    DownloadStream() = default;
    DownloadStream(const DownloadStream&) = delete;
    DownloadStream& operator=(const DownloadStream&) = delete;
    virtual ~DownloadStream() = default;
    // 已完成 pin/open/type/length 检查才返回流。范围必须在描述长度内，count 1..64KiB。
    // writer 未消费完返回块不得再次 pull；取消后拒绝新读，迟到任务仍退休。
    virtual TaskSubmission pull(UploadOffset, std::size_t count, OwnedTransferBlock& buffer,
                                Completion<OwnedTransferBlock>) = 0;
    // 仅报告 writer 实际 acceptedBytes，不能重复报告或超过上一块剩余量。
    // 全块消费前拒绝下一次 pull；0不刷新无进展计时。
    virtual void consumed(std::size_t bytes) noexcept = 0;
    // owner-loop通知禁用/无进展/停止取消；回调不得拥有stream，须幂等且无阻塞。
    virtual void setCancellationCallback(std::function<void()>) = 0;
    virtual void cancel() noexcept = 0;
};
struct OpenedDownload {
    DownloadDescriptor descriptor;
    std::unique_ptr<DownloadStream> stream;
};
class UploadDispatcher {
public:
    virtual ~UploadDispatcher() = default;
    // 与 IdentityDispatcher 相同：自有命令，接受后恰一次延后完成，拒绝无回调。
    virtual TaskSubmission submit(RequestContext, CreateUploadCommand, Completion<UploadCreated>) = 0;
    virtual TaskSubmission submit(RequestContext, ListUploadsCommand, Completion<Page<UploadView, UploadCursor>>) = 0;
    virtual TaskSubmission submit(RequestContext, GetUploadCommand, Completion<UploadView>) = 0;
    virtual TaskSubmission submit(RequestContext, CancelUploadCommand, Completion<void>) = 0;
    virtual TaskSubmission submit(RequestContext, CompleteUploadCommand, Completion<UploadCompletionAccepted>) = 0;
    virtual TaskSubmission submit(RequestContext, PreparePatchCommand, Completion<PatchPreparation>) = 0;
    virtual TaskSubmission submit(RequestContext, GetStorageCommand, Completion<StorageView>) = 0;
    virtual TaskSubmission submit(RequestContext, SetStorageBudgetCommand, Completion<StorageView>) = 0;
};
class TransferDispatcher : public UploadDispatcher {
public:
    using UploadDispatcher::submit;
    virtual TaskSubmission submit(RequestContext, OpenDownloadCommand, Completion<OpenedDownload>) = 0;
};
}  // namespace anof
