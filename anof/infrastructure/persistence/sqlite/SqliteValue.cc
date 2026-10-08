#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

#include <algorithm>
#include <limits>

namespace anof::infrastructure::sqlite {

Result<UtcTime> readTime(const SqliteStatement& statement, int column) {
    static_assert(std::numeric_limits<std::chrono::microseconds::rep>::digits >= 63);
    auto value = statement.integer(column);
    if (!value) return value.error();
    return UtcTime{ std::chrono::microseconds{ value.value() } };
}

Result<std::array<std::uint8_t, 32>> readDigest(const SqliteStatement& statement, int column) {
    auto bytes = statement.blob(column);
    if (!bytes) return bytes.error();
    if (bytes.value().size() != 32) return ErrorCode::IntegrityError;
    std::array<std::uint8_t, 32> result{};
    std::copy(bytes.value().begin(), bytes.value().end(), result.begin());
    return result;
}

Result<SessionDigest> readSessionDigest(const SqliteStatement& statement, int column) {
    auto bytes = readDigest(statement, column);
    if (!bytes) return bytes.error();
    return SessionDigest(bytes.value());
}

Result<bool> readBoolean(const SqliteStatement& statement, int column) {
    auto value = statement.integer(column);
    if (!value) return value.error();
    if (value.value() != 0 && value.value() != 1) return ErrorCode::IntegrityError;
    return value.value() == 1;
}

Result<UserRole> readUserRole(const SqliteStatement& statement, int column) {
    auto value = statement.integer(column);
    if (!value) return value.error();
    if (value.value() == 0) return UserRole::Member;
    if (value.value() == 1) return UserRole::Administrator;
    return ErrorCode::IntegrityError;
}

Result<SpaceKind> readSpaceKind(const SqliteStatement& statement, int column) {
    auto value = statement.integer(column);
    if (!value) return value.error();
    if (value.value() == 0) return SpaceKind::Private;
    if (value.value() == 1) return SpaceKind::Public;
    return ErrorCode::IntegrityError;
}

}  // namespace anof::infrastructure::sqlite
