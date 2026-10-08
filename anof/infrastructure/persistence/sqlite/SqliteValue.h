#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/domain/drive/Space.h>
#include <anof/domain/identity/Principal.h>
#include <anof/domain/identity/SessionDigest.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>

namespace anof::infrastructure::sqlite {

template <typename IdType>
Result<IdType> readId(const SqliteStatement& statement, int column) {
    auto text = statement.text(column);
    if (!text) return text.error();
    auto value = IdType::parse(text.value());
    if (!value || value.value().toString() != text.value()) return ErrorCode::IntegrityError;
    return std::move(value).value();
}

template <typename QuantityType>
Result<QuantityType> readQuantity(const SqliteStatement& statement, int column) {
    auto integer = statement.integer(column);
    if (!integer) return integer.error();
    auto value = QuantityType::from(integer.value());
    if (!value) return ErrorCode::IntegrityError;
    return value.value();
}

Result<UtcTime> readTime(const SqliteStatement& statement, int column);
Result<SessionDigest> readSessionDigest(const SqliteStatement& statement, int column);
Result<std::array<std::uint8_t, 32>> readDigest(const SqliteStatement& statement, int column);
Result<bool> readBoolean(const SqliteStatement& statement, int column);
Result<UserRole> readUserRole(const SqliteStatement& statement, int column);
Result<SpaceKind> readSpaceKind(const SqliteStatement& statement, int column);

}  // namespace anof::infrastructure::sqlite
