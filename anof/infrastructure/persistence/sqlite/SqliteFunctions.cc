#include <anof/infrastructure/persistence/sqlite/SqliteFunctions.h>
#include <anof/infrastructure/persistence/sqlite/SqliteError.h>
#include <anof/core/Id.h>
#include <anof/domain/identity/IdentityValues.h>
#include <anof/domain/identity/Secrets.h>
#include <sqlite3.h>
#include <new>
#include <string_view>

namespace anof::infrastructure::sqlite {
namespace {
template <int Kind>
void validate(sqlite3_context* context, int, sqlite3_value** values) noexcept {
    try {
        auto* value = values[0];
        const int maximum = Kind == 0 ? 36 : Kind == 1 ? 32 : Kind == 2 ? 128 : 512;
        if (sqlite3_value_type(value) != SQLITE_TEXT || sqlite3_value_bytes(value) > maximum) {
            sqlite3_result_int(context, 0);
            return;
        }
        const auto* bytes = sqlite3_value_text(value);
        if (!bytes) { sqlite3_result_error_nomem(context); return; }
        const std::string_view text(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(sqlite3_value_bytes(value)));
        bool valid = false;
        if constexpr (Kind == 0) {
            auto parsed = Id<struct PersistentUuid>::parse(text);
            valid = parsed && parsed.value().toString() == text;
        } else if constexpr (Kind == 1) {
            auto parsed = LoginName::parse(text);
            valid = parsed && parsed.value().value() == text;
        } else if constexpr (Kind == 2) {
            valid = static_cast<bool>(DisplayName::parse(text));
        } else {
            // schema 的硬上限；调用方读取时仍须施加显式、更小的执行预算。
            auto parsed = EncodedPasswordHash::parse(text, { 262144, 10, 1 });
            const auto saltStart = text.find('$', 16);
            const auto hashStart = saltStart == text.npos ? text.npos : text.find('$', saltStart + 1);
            valid = parsed && saltStart != text.npos && hashStart != text.npos &&
                    hashStart - saltStart - 1 == 22 && text.size() - hashStart - 1 == 43;
        }
        sqlite3_result_int(context, valid ? 1 : 0);
    } catch (const std::bad_alloc&) {
        sqlite3_result_error_nomem(context);
    } catch (...) {
        sqlite3_result_error(context, "validation failed", -1);
    }
}
}
Result<void> registerSqliteFunctions(sqlite3* handle) {
    const struct { const char* name; void (*function)(sqlite3_context*, int, sqlite3_value**); } functions[] = {
        { "anof_uuid", validate<0> }, { "anof_login", validate<1> },
        { "anof_display_name", validate<2> }, { "anof_password_hash", validate<3> }
    };
    for (const auto& item : functions) {
        const int code = sqlite3_create_function_v2(handle, item.name, 1,
            SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, nullptr, item.function, nullptr, nullptr, nullptr);
        if (code != SQLITE_OK) return sqliteError(code, SqliteOperation::Configure);
    }
    return {};
}
}  // namespace anof::infrastructure::sqlite
