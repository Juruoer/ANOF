#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <sqlite3.h>
#include <type_traits>

namespace anof::infrastructure::sqlite::detail {
Result<std::unique_ptr<SqliteStatement>> query(SqliteTransactionContext& context, std::string_view sql, std::initializer_list<Parameter> parameters) {
    auto ready = context.requireOpen();
    if (!ready) return ready.error();
    auto statement = SqliteStatement::prepare(context.connection(), sql);
    if (!statement) return context.fail(statement.error());
    int index = 1;
    for (const auto& p : parameters) {
        auto bound = std::visit([&](auto value) -> Result<void> {
            using T = decltype(value);
            if constexpr (std::is_same_v<T, std::nullptr_t>) return statement.value()->bindNull(index);
            else if constexpr (std::is_same_v<T, std::int64_t>) return statement.value()->bindInt64(index, value);
            else if constexpr (std::is_same_v<T, std::string_view>) return statement.value()->bindText(index, value);
            else return statement.value()->bindBlob(index, value.data, value.size);
        }, p.value);
        if (!bound) return context.fail(bound.error());
        ++index;
    }
    return std::move(statement).value();
}

Result<std::int64_t> write(SqliteTransactionContext& context, std::string_view sql, std::initializer_list<Parameter> parameters) {
    auto statement = query(context, sql, parameters);
    if (!statement) return statement.error();
    auto row = statement.value()->step();
    if (!row) return context.fail(row.error());
    if (row.value()) return context.fail(ErrorCode::IntegrityError);
    return static_cast<std::int64_t>(sqlite3_changes64(context.connection().handle()));
}

Result<void> one(SqliteTransactionContext& context, const Result<std::int64_t>& count) {
    if (!count) return count.error();
    if (count.value() != 1) return context.fail(ErrorCode::IntegrityError);
    return {};
}
}
