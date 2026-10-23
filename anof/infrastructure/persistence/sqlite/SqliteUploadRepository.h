#pragma once
#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/application/ports/UploadRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteUploadRepository final : public DriveUploadRepository {
public:
    explicit SqliteUploadRepository(SqliteTransactionContext& c) : context_(c) {}
    Result<std::optional<Upload>> find(UploadId) override;
    Result<std::optional<Upload>> findByClientId(UserId, OperationId) override;
    Result<UploadCounts> counts(UserId) override;
    Result<Page<Upload, UploadPosition>> list(UserId, PageSize, std::optional<UploadPosition>) override;
    Result<Page<Upload, UploadPosition>> scan(UploadScan, UtcTime, PageSize, std::optional<UploadPosition>) override;
    Result<ByteCount> unwrittenCommitment() override;
    Result<void> insert(const Upload&) override;
    Result<void> replace(UploadId, UploadMatch, const Upload&) override;
    Result<std::vector<UploadId>> requestUserCancellation(UserId) override;
    Result<void> eraseTerminal(UploadId, UploadGeneration, UtcTime) override;

    Result<NodeCount> markResultsDeleted(const std::vector<NodeId>&, UtcTime) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
