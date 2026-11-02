#pragma once
#include <anof/application/ports/DriveAuditRepository.h>
#include <anof/application/ports/MaintenanceStateReader.h>
#include <anof/application/ports/NodeAccountingRepository.h>
#include <anof/application/ports/TransferMetadataStore.h>
#include <anof/application/ports/TrashRepository.h>
#include <anof/application/ports/TreeRepository.h>
namespace anof {
class BlobLifecycleReader {
public:
    virtual ~BlobLifecycleReader() = default;
    virtual Result<std::optional<BlobRecord>> record(BlobId) = 0;
    virtual Result<Page<BlobRecord, BlobGcPosition>> gcCandidates(UtcTime, PageSize, std::optional<BlobGcPosition>) = 0;
    virtual Result<Page<BlobRecord, BlobId>> deleting(PageSize, std::optional<BlobId>) = 0;
    virtual Result<GcStatistics> gcStatistics() = 0;
};
class LifecycleBlobRepository : public BlobRepository, public BlobLifecycleReader {
public:
    // 复验实际引用；已有无引用时间不可重复刷新，返回权威记录。
    virtual Result<BlobRecord> markUnreferenced(BlobId, Revision, UtcTime) = 0;
    // owner loop持排他资格；仓储仍复验引用、发布持有、时间和revision。
    virtual Result<BlobRecord> markDeleting(BlobId, Revision, UtcTime) = 0;
    virtual Result<void> eraseDeleting(BlobId, Revision) = 0;
};
class DriveQuotaRepository : public ReservationQuotaRepository {
public:
    virtual Result<void> releaseUsed(const QuotaScope&, QuotaDelta) = 0;
};
class DriveStorageRepository : public StorageRepository {
public:
    virtual Result<void> releaseRetained(ByteCount) = 0;
};
class DriveUploadRepository : public UploadRepository {
public:
    // 同永久删除事务，批量<=1000；只更新COMPLETED；保留首次证据，generation溢出整体失败。
    // 时间不早于终态及传入的树事件时间。返回实际首次修改数，不因已删除证据再次递增。
    virtual Result<NodeCount> markResultsDeleted(const std::vector<NodeId>&, UtcTime) = 0;
};
class DriveReadSnapshot : public TransferReadSnapshot {
public:
    virtual MaintenanceStateReader& maintenanceState() = 0;
    virtual TreeReader& tree() = 0;
    virtual TrashReader& trash() = 0;
    virtual BlobLifecycleReader& blobLifecycle() = 0;
    virtual NodeAccountingReader& nodeAccounting() = 0;
};
class DriveTransaction : public TransferTransaction {
public:
    virtual TreeRepository& tree() = 0;
    virtual TrashRepository& trash() = 0;
    virtual LifecycleBlobRepository& blobs() override = 0;
    virtual DriveQuotaRepository& quotas() override = 0;
    virtual DriveStorageRepository& storage() override = 0;
    virtual DriveUploadRepository& uploads() override = 0;
    virtual NodeAccountingRepository& nodeAccounting() = 0;
    virtual DriveAuditRepository& driveAudit() = 0;
};
class DriveMetadataStore : public TransferMetadataStore {
public:
    // 与base/transfer视图共用连接和活动事务限制；不允许独立开库拼接。
    virtual Result<std::unique_ptr<DriveReadSnapshot>> beginDriveRead() = 0;
    virtual Result<std::unique_ptr<DriveTransaction>> beginDriveWrite() = 0;
};
}  // namespace anof
