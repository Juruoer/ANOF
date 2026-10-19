#pragma once
#include <anof/application/ports/TransferDispatcher.h>
#include <anof/domain/transfer/TransferValues.h>
#include <anof/runtime/DiskExecutor.h>
#include <anof/runtime/MetadataExecutor.h>

namespace anof::runtime {
// owner-loop 上传协调器；磁盘与 metadata 分阶段执行，持久化 generation 约束迟到结果。
class UploadExecutor final : public TransferDispatcher {
public:
    UploadExecutor(muduox::net::EventLoop&, MetadataExecutor&, DiskExecutor&, DiskSafetyPolicy);
    ~UploadExecutor() override;
    void recover(std::function<void(Result<void>)>);
    // 用户禁用提交后触发扫描；持久取消标志是权威依据。
    void notifyUserDisabled(UserId, bool confirmed = true);
    bool blobPinned(BlobId) const;
    void requestStop(std::function<void()>);
    bool stopped() const;
    TaskSubmission submit(RequestContext, OpenDownloadCommand, Completion<OpenedDownload>) override;
    TaskSubmission submit(RequestContext, CreateUploadCommand, Completion<UploadCreated>) override;
    TaskSubmission submit(RequestContext, ListUploadsCommand, Completion<Page<UploadView, UploadCursor>>) override;
    TaskSubmission submit(RequestContext, GetUploadCommand, Completion<UploadView>) override;
    TaskSubmission submit(RequestContext, CancelUploadCommand, Completion<void>) override;
    TaskSubmission submit(RequestContext, CompleteUploadCommand, Completion<UploadCompletionAccepted>) override;
    TaskSubmission submit(RequestContext, PreparePatchCommand, Completion<PatchPreparation>) override;
    TaskSubmission submit(RequestContext, GetStorageCommand, Completion<StorageView>) override;
    TaskSubmission submit(RequestContext, SetStorageBudgetCommand, Completion<StorageView>) override;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}  // namespace anof::runtime
