#pragma once
#include <anof/application/ports/TrashRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteTrashRepository final : public TrashRepository {
public:
    explicit SqliteTrashRepository(SqliteTransactionContext& c) : context_(c) {}
    Result<std::optional<TrashEntry>> find(TrashId) override;
    Result<Page<TrashEntry, TrashPosition>> list(const TrashQuery&) override;
    Result<Page<TrashEntry, TrashExpiryPosition>> expired(UtcTime, PageSize, std::optional<TrashExpiryPosition>) override;
    Result<void> insert(const TrashEntry&) override;
    Result<void> erase(TrashCondition) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
