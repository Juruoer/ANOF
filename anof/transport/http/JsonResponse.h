#pragma once

#include <anof/application/drive/CreateDirectory.h>
#include <muduox/net/http/HttpResponse.h>

#include <cstddef>
#include <string>

namespace anof::http {

inline constexpr std::size_t kMaxJsonBytes = 64 * 1024;
enum class ProtocolError {
    InvalidArgument,
    InvalidName,
    Unauthenticated,
    NotFound,
    ParentNotDirectory,
    DepthLimitExceeded,
    NameConflict,
    QuotaExceeded,
    ConcurrentModification,
    RequestTooLarge,
    UnsupportedMediaType,
    RateLimited,
    CommitOutcomeUnknown,
    InternalError,
    TemporarilyUnavailable,
    StorageFull
};

ProtocolError protocolError(ErrorCode code);

// 流式发送时 headers 不含 body，长度取 body.size()；提前拒绝可显式转为缓冲响应。
struct JsonResponse {
    muduox::net::HttpResponse headers{ false };
    std::string body;
    muduox::net::HttpResponse buffered(bool close) const;
};

JsonResponse errorResponse(ProtocolError error, RequestId requestId);
Result<JsonResponse> createdDirectoryResponse(const CreateDirectoryResult& result, RequestId requestId);
JsonResponse healthResponse(bool ready, RequestId requestId);
JsonResponse liveResponse(RequestId requestId);
Result<std::string> formatUtcTimestamp(UtcTime time);

}  // namespace anof::http
