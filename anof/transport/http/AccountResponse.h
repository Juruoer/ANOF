#pragma once

#include <anof/application/identity/AccountCommands.h>
#include <anof/transport/http/IdentityResponse.h>

namespace anof::http {
// 本人改名复用 currentUserResponse；改密复用 noContentResponse。
Result<JsonResponse> adminUserResponse(const AdminUserView& user, RequestId requestId);
Result<JsonResponse> managedQuotaResponse(const ManagedQuota& quota, RequestId requestId);
Result<JsonResponse> usersResponse(const Page<UserProfile, UserCursor>& page, RequestId requestId);
// 仅额度 PUT 使用；mkdir 和其他操作保持原映射。
ProtocolError quotaUpdateError(ErrorCode code);
}  // namespace anof::http
