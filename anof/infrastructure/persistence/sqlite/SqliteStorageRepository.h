#pragma once
#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/application/ports/StorageRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteStorageRepository final : public DriveStorageRepository {
public:
    explicit SqliteStorageRepository(SqliteTransactionContext& c) : context_(c) {}
    Result<StorageSnapshot> get() override;
    Result<StorageSnapshot> setBudget(Revision, ByteCount) override;
    Result<void> reserve(ByteCount) override;
    Result<void> releaseReservation(ByteCount) override;
    Result<void> publishReservation(ByteCount) override;

    Result<void> releaseRetained(ByteCount) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
