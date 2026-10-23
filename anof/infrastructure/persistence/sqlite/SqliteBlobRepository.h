#pragma once
#include <anof/application/ports/BlobRepository.h>
#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteBlobRepository final : public LifecycleBlobRepository {
public:
    explicit SqliteBlobRepository(SqliteTransactionContext& c) : context_(c) {}
    Result<std::optional<Blob>> find(BlobId) override;
    Result<std::optional<Blob>> findByNode(NodeId) override;
    Result<std::optional<BlobId>> candidateForUpload(UploadId) override;
    Result<bool> hasNodeReference(BlobId) override;
    Result<bool> hasUploadReference(BlobId) override;
    Result<Page<Blob, BlobId>> list(PageSize, std::optional<BlobId>) override;
    Result<void> insert(const BlobRecord&) override;

    Result<std::optional<BlobRecord>> record(BlobId) override;
    Result<Page<BlobRecord, BlobGcPosition>> gcCandidates(UtcTime, PageSize, std::optional<BlobGcPosition>) override;
    Result<Page<BlobRecord, BlobId>> deleting(PageSize, std::optional<BlobId>) override;
    Result<GcStatistics> gcStatistics() override;
    Result<BlobRecord> markUnreferenced(BlobId, Revision, UtcTime) override;
    Result<BlobRecord> markDeleting(BlobId, Revision, UtcTime) override;
    Result<void> eraseDeleting(BlobId, Revision) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
