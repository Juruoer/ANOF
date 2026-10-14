#include <anof/transport/http/internal/IdentityJson.h>
#include <anof/transport/http/JsonResponse.h>

namespace anof::http::internal {
using Json = nlohmann::json;
std::string userTag(const UserProfile& user) {
    return "\"user-" + user.id.toString() + "-" + std::to_string(user.revision.value()) + "\"";
}
Result<Json> userJson(const UserProfile& user) {
    auto created = formatUtcTimestamp(user.createdAt);
    auto updated = formatUtcTimestamp(user.updatedAt);
    if (!created || !updated || !LoginName::parse(user.login.value()) || !DisplayName::parse(user.displayName.value())) return ErrorCode::InternalError;
    const char* role = nullptr;
    switch (user.role) {
    case UserRole::Member: role = "member"; break;
    case UserRole::Administrator: role = "administrator"; break;
    }
    if (!role) return ErrorCode::InternalError;
    return Json{{"id", user.id.toString()}, {"login", user.login.value()}, {"display_name", user.displayName.value()}, {"role", role}, {"enabled", user.enabled}, {"revision", std::to_string(user.revision.value())}, {"created_at", created.value()}, {"updated_at", updated.value()}};
}
Json amountJson(const QuotaDelta& amount) {
    return {{"bytes", std::to_string(amount.bytes.value())}, {"nodes", std::to_string(amount.nodes.value())}};
}
Json quotaJson(const QuotaSnapshot& quota) {
    return {{"limit", amountJson(quota.state.limit)}, {"used", amountJson(quota.state.used)}, {"reserved", amountJson(quota.state.reserved)}, {"revision", std::to_string(quota.revision.value())}};
}
}  // namespace anof::http::internal
