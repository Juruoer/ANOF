#pragma once
#include <anof/application/ports/BlobRepository.h>
#include <anof/application/ports/MetadataStore.h>
#include <anof/application/ports/StorageRepository.h>
#include <anof/application/ports/TransferAuditRepository.h>
#include <anof/application/ports/UploadRepository.h>

namespace anof {
struct NewFile {
    NewDirectory metadata;
    BlobId blobId;
};
class FileNodeRepository : public NodeRepository {
public:
    // Blob 必须存在；同空间活动父目录、唯一名称。父 revision 由调用者同事务推进。
    virtual Result<NodeDetails> insertFile(const NewFile&) = 0;
};
class ReservationQuotaRepository : public QuotaRepository {
public:
    virtual Result<void> reserve(const QuotaScope&, QuotaDelta) = 0;
    virtual Result<void> commitReservation(const QuotaScope&, QuotaDelta) = 0;
    virtual Result<void> releaseReservation(const QuotaScope&, QuotaDelta) = 0;
};
// 接口隔离：完整继承身份/目录事务，新增能力不是另开连接或另开事务。
// 同一适配器应实现这些派生接口；不能将两个独立 MetadataStore 的事务拼接。
class TransferReadSnapshot : public MetadataReadSnapshot {
public:
    virtual UploadReader& uploads() = 0;
    virtual BlobReader& blobs() = 0;
    virtual StorageReader& storage() = 0;
};
class TransferTransaction : public MetadataTransaction {
public:
    virtual FileNodeRepository& nodes() override = 0;
    virtual ReservationQuotaRepository& quotas() override = 0;
    virtual UploadRepository& uploads() = 0;
    virtual BlobRepository& blobs() = 0;
    virtual StorageRepository& storage() = 0;
    virtual TransferAuditRepository& transferAudit() = 0;
};
class TransferMetadataStore : public MetadataStore {
public:
    virtual ~TransferMetadataStore() = default;
    // 返回非空；一次只允许一个活动快照/事务；与 MetadataStore 视图共享此限制。
    virtual Result<std::unique_ptr<TransferReadSnapshot>> beginTransferRead() = 0;
    virtual Result<std::unique_ptr<TransferTransaction>> beginTransferWrite() = 0;
};
}  // namespace anof
