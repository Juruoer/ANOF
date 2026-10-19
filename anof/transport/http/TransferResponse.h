#pragma once
#include <anof/application/ports/TransferDispatcher.h>
#include <anof/transport/http/JsonResponse.h>
namespace anof::http {
ProtocolError transferError(ErrorCode);
Result<JsonResponse> uploadResponse(const UploadView&, RequestId);
Result<JsonResponse> uploadHeadResponse(const UploadView&, RequestId);
Result<JsonResponse> uploadCreatedResponse(const UploadCreated&, RequestId);
Result<JsonResponse> uploadCompleteResponse(const UploadCompletionAccepted&, RequestId);
Result<JsonResponse> uploadsResponse(const Page<UploadView, UploadCursor>&, RequestId);
Result<JsonResponse> storageResponse(const StorageView&, RequestId);
JsonResponse patchResponse(UploadOffset, RequestId);
JsonResponse patchConflictResponse(PatchOffsetConflict, RequestId);
}  // namespace anof::http
