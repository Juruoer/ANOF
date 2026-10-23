#pragma once
#include <anof/application/drive/DriveCommands.h>
#include <anof/transport/http/IdentityResponse.h>
namespace anof::http {
Result<JsonResponse> copiedNodeResponse(const NodeDetails&, RequestId);
Result<JsonResponse> restoredNodeResponse(const NodeDetails&, RequestId);
Result<JsonResponse> trashResponse(const TrashView&, RequestId);
Result<JsonResponse> trashedNodeResponse(const TrashView&, RequestId);
Result<JsonResponse> searchResponse(const Page<NodeDetails, SearchCursor>&, RequestId);
Result<JsonResponse> trashPageResponse(const Page<TrashView, TrashCursor>&, RequestId);
}  // namespace anof::http
