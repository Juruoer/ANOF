#pragma once

#include <anof/application/identity/IdentityCommands.h>
#include <anof/transport/http/JsonResponse.h>

namespace anof::http {
// 最多 200 项；这是整份分页 JSON 的存储上限，不是 writer 的单块发送上限。
inline constexpr std::size_t kMaxIdentityResponseBytes = 512 * 1024;
Result<JsonResponse> registeredUserResponse(const UserProfile& user, RequestId requestId);
Result<JsonResponse> currentUserResponse(const UserProfile& user, RequestId requestId);
// 消费只移动的秘密；仅成功响应正文持有明文。调用方须在发送/取消终态清理正文。
Result<JsonResponse> loginResponse(LoginResult result, RequestId requestId);
Result<JsonResponse> createdInviteResponse(CreatedInvite result, RequestId requestId);
JsonResponse noContentResponse(RequestId requestId);
Result<JsonResponse> spacesResponse(const std::vector<SpaceView>& spaces, RequestId requestId);
Result<JsonResponse> nodeResponse(const NodeDetails& node, RequestId requestId);
Result<JsonResponse> childrenResponse(const Page<NodeDetails, ChildrenCursor>& page, RequestId requestId);
Result<JsonResponse> invitesResponse(const Page<InviteInfo, InviteCursor>& page, RequestId requestId);
}  // namespace anof::http
