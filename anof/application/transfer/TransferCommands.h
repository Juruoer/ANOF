#pragma once
#include <anof/application/RequestContext.h>
#include <anof/core/Page.h>
#include <anof/domain/transfer/Upload.h>
#include <anof/domain/transfer/Storage.h>
#include <anof/domain/drive/Blob.h>

namespace anof {
struct CreateUploadCommand { OperationId clientUploadId; UploadCreateParameters parameters; };
struct UploadCursor { UserId userId; UploadPosition position; };
struct ListUploadsCommand {
    PageSize size;
    std::optional<UploadCursor> after;
    std::optional<OperationId> clientUploadId; // 与 after 互斥。
};
struct GetUploadCommand { UploadId id; };
struct CancelUploadCommand { UploadId id; };
struct CompleteUploadCommand { UploadId id; std::optional<UploadTarget> target; };
struct PreparePatchCommand { UploadId id; UploadOffset offset; ByteCount length; };
// HEAD 同样打开并核对文件，但不占传输槽，返回空 stream。
struct OpenDownloadCommand { NodeId id; bool metadataOnly = false; };
struct GetStorageCommand {};
struct StorageCondition { InstanceId instance; Revision revision; };
struct SetStorageBudgetCommand { StorageCondition condition; ByteCount budget; };
// wire DTO 不泄露 generation、内部路径、取消标志及清理责任。
struct UploadView {
    InstanceId instanceId;
    UploadId id;
    OperationId clientUploadId;
    SpaceId spaceId;
    UploadTarget target;
    FileLength length;
    UploadOffset offset;
    std::optional<Sha256> expectedSha256;
    std::optional<Sha256> actualSha256;
    UploadState state;
    std::optional<NodeId> resultNodeId;
    UtcTime createdAt;
    UtcTime lastProgressAt;
    UtcTime expiresAt;
    std::optional<UtcTime> terminalAt;
    std::optional<TransferFailure> lastError;
};
enum class UploadCreateDisposition { Created, Existing, Creating };
struct UploadCreated { UploadView upload; UploadCreateDisposition disposition; };
enum class UploadCompleteDisposition { Accepted, Completed };
struct UploadCompletionAccepted { UploadView upload; UploadCompleteDisposition disposition; };
struct DiskSample { ByteCount free; ByteCount safetyMargin; UtcTime sampledAt; };
struct StorageView {
    InstanceId instanceId;
    StorageSnapshot accounting;
    // null=UNAVAILABLE，三个采样字段一起有/无；预算事务与磁盘采样非同一快照。
    std::optional<DiskSample> disk;
};
struct DownloadDescriptor { NodeId nodeId; std::string name; Blob blob; };
}  // namespace anof
