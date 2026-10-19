#pragma once
#include <anof/application/ports/BlobRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteBlobRepository final : public BlobRepository {
public:
    explicit SqliteBlobRepository(SqliteTransactionContext& c) : context_(c) {}
    Result<std::optional<Blob>> find(BlobId) override;
    Result<std::optional<Blob>> findByNode(NodeId) override;
    Result<std::optional<BlobId>> candidateForUpload(UploadId) override;
    Result<bool> hasNodeReference(BlobId) override;
    Result<bool> hasUploadReference(BlobId) override;
    Result<Page<Blob, BlobId>> list(PageSize, std::optional<BlobId>) override;
    Result<void> insert(const Blob&) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
