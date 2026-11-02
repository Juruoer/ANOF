#pragma once

#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

#include <initializer_list>
#include <variant>

namespace anof::infrastructure::sqlite::detail {
struct BlobParameter {
    const void* data;
    std::size_t size;
};
// 仅在本次绑定调用中借用；SQLite 绑定使用 TRANSIENT，不保存调用方视图。
struct Parameter {
    std::variant<std::nullptr_t, std::int64_t, std::string_view, BlobParameter> value;
    Parameter(std::nullptr_t) : value(nullptr) {}
    Parameter(std::int64_t n) : value(n) {}
    Parameter(std::string_view s) : value(s) {}
    Parameter(const std::string& s) : value(std::string_view(s)) {}
    Parameter(const char* s) : value(std::string_view(s)) {}
    Parameter(BlobParameter b) : value(b) {}
};
Result<std::unique_ptr<SqliteStatement>> query(SqliteTransactionContext&, std::string_view, std::initializer_list<Parameter> = {});
Result<std::int64_t> write(SqliteTransactionContext&, std::string_view, std::initializer_list<Parameter> = {});
Result<void> one(SqliteTransactionContext&, const Result<std::int64_t>&);
}  // namespace anof::infrastructure::sqlite::detail
