#include <anof/transport/http/AccountResponse.h>
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/internal/IdentityJson.h>
#include <anof/transport/http/internal/JsonOutput.h>

#include <type_traits>

namespace anof::http {
namespace {
using Json = nlohmann::json;
JsonResponse response(RequestId id, const Json& json) {
    return internal::jsonResponse(200, "OK", id, json, kMaxIdentityResponseBytes);
}
}  // namespace
Result<JsonResponse> adminUserResponse(const AdminUserView& user, RequestId id) {
    auto json = internal::userJson(user.user);
    if (!json) return json.error();
    auto output = response(id, { { "user", std::move(json).value() }, { "private_space_id", user.privateSpaceId.toString() } });
    output.headers.addHeader("ETag", internal::userTag(user.user));
    return output;
}
Result<JsonResponse> managedQuotaResponse(const ManagedQuota& quota, RequestId id) {
    return std::visit([&](const auto& scope) -> Result<JsonResponse> {
        Json user = nullptr;
        std::string tag;
        if constexpr (std::is_same_v<std::decay_t<decltype(scope)>, SpaceQuota>) {
            tag = "\"quota-space-" + scope.spaceId.toString();
        } else {
            user = scope.userId.toString();
            tag = "\"quota-contribution-" + scope.spaceId.toString() + "-" + scope.userId.toString();
        }
        tag += "-" + std::to_string(quota.quota.revision.value()) + "\"";
        auto output = response(id, { { "space_id", scope.spaceId.toString() }, { "user_id", std::move(user) }, { "quota", internal::quotaJson(quota.quota) } });
        output.headers.addHeader("ETag", tag);
        return output;
    },
                      quota.scope);
}
Result<JsonResponse> usersResponse(const Page<UserProfile, UserCursor>& page, RequestId id) {
    if (page.items.size() > 200 || (page.items.empty() && page.next)) return ErrorCode::InternalError;
    Json items = Json::array();
    for (const auto& user : page.items) {
        auto json = internal::userJson(user);
        if (!json) return json.error();
        items.push_back(std::move(json).value());
    }
    Json next = nullptr;
    if (page.next) {
        if (page.next->position.id != page.items.back().id) return ErrorCode::InternalError;
        auto cursor = encodeCursor(*page.next);
        if (!cursor) return cursor.error();
        next = std::move(cursor).value();
    }
    return response(id, { { "items", std::move(items) }, { "next_cursor", std::move(next) } });
}
ProtocolError quotaUpdateError(ErrorCode code) {
    return code == ErrorCode::QuotaExceeded ? ProtocolError::QuotaLimitExceeded : protocolError(code);
}
}  // namespace anof::http
