#pragma once
#include <anof/application/ports/NodeIdGenerator.h>
#include <anof/application/ports/TransferIdGenerator.h>
#include <anof/application/ports/TransferMetadataStore.h>
#include <anof/application/transfer/TransferCommands.h>

namespace anof {
struct UploadCreation {
    Upload upload;
    bool inserted;
};
struct UploadPatch {
    Upload upload;
    UploadOffset end;
};
// 每次调用只持有短 metadata 事务，不做磁盘、哈希或等待。跨阶段必须匹配 generation。
class UploadService {
public:
    UploadService(TransferMetadataStore& store, Clock& clock, TransferIdGenerator& ids, NodeIdGenerator& nodes)
        : store_(store), clock_(clock), ids_(ids), nodes_(nodes) {}
    Result<std::optional<Upload>> findReplay(const RequestContext&, const CreateUploadCommand&);
    Result<UploadCreation> create(const RequestContext&, const CreateUploadCommand&, ByteCount free, ByteCount margin);
    Result<UploadView> get(const RequestContext&, UploadId);
    Result<Page<UploadView, UploadCursor>> list(const RequestContext&, const ListUploadsCommand&);
    Result<Upload> read(UploadId);  // 内部权威重读；不直接暴露给 HTTP。
    Result<UploadView> view(const Upload&);
    Result<Upload> refresh(const RequestContext&, UploadId);
    Result<Upload> opened(UploadId, UploadGeneration);
    Result<UploadPatch> preparePatch(const RequestContext&, const PreparePatchCommand&);
    Result<Upload> commitPatch(const RequestContext&, const UploadPatch&);
    Result<Upload> acceptComplete(const RequestContext&, const CompleteUploadCommand&);
    Result<Upload> recordVerification(UploadId, UploadGeneration, Sha256, RequestId);
    Result<Upload> preparePublish(UploadId, UploadGeneration);
    Result<Upload> commitPublish(UploadId, UploadGeneration, RequestId);
    Result<Upload> markObject(UploadId, UploadGeneration);
    Result<Upload> publicationConflict(UploadId, UploadGeneration, TransferFailure);
    Result<Upload> cancel(const RequestContext&, UploadId);
    Result<Upload> terminate(UploadId, UploadGeneration, UploadState, TransferFailure, RequestId);
    Result<Upload> confirmCleanup(UploadId, UploadGeneration);
    Result<StorageView> storage(const RequestContext&);
    Result<StorageView> setBudget(const RequestContext&, const SetStorageBudgetCommand&);

private:
    TransferMetadataStore& store_;
    Clock& clock_;
    TransferIdGenerator& ids_;
    NodeIdGenerator& nodes_;
};
}  // namespace anof
