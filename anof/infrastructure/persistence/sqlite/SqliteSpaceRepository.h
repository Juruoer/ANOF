#pragma once

#include <anof/application/ports/SpaceRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {

class SqliteSpaceRepository final : public SpaceRepository {
public:
    explicit SqliteSpaceRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<std::optional<Space>> find(SpaceId id) override;
    Result<std::optional<Space>> findPrivate(UserId) override;
    Result<std::vector<Space>> listAccessible(UserId) override;
    Result<void> insert(const Space&) override;
private:
    SqliteTransactionContext& context_;
};

}  // namespace anof::infrastructure::sqlite
