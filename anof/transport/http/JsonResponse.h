#pragma once

#include <anof/application/drive/CreateDirectory.h>
#include <anof/application/ports/TaskDispatcher.h>
#include <muduox/net/http/HttpResponse.h>

#include <cstddef>
#include <string>

namespace anof::http {

inline constexpr std::size_t kMaxJsonBytes = 64 * 1024;
enum class ProtocolError {
    CookieModeDisabled,
    RequestOriginRejected,
    CsrfRequired,
    IntegrityError,
    LoginConflict,
    InviteUnavailable,
    UserLimitReached,
    Forbidden,
    PreconditionFailed,
    PreconditionRequired,
    SessionLimitReached,
    InvalidArgument,
    InvalidName,
    Unauthenticated,
    NotFound,
    ParentNotDirectory,
    DepthLimitExceeded,
    NameConflict,
    QuotaExceeded,
    QuotaLimitExceeded,
    ConcurrentModification,
    RequestTooLarge,
    UnsupportedMediaType,
    RateLimited,
    CommitOutcomeUnknown,
    InternalError,
    TemporarilyUnavailable,
    StorageFull,
    TransferStorageFull,
    LengthRequired,
    UploadKeyConflict,
    UploadOffsetMismatch,
    UploadLengthMismatch,
    UploadBusy,
    UploadIncomplete,
    UploadStateConflict,
    UploadCommitInProgress,
    StorageBudgetExceeded,
    UploadTerminated,
    UploadLimitReached,
    NotAFile,
    CrossSpaceMove,
    DirectoryCycle,
    TreeLimitExceeded,
    NodeLimitReached,
    RestoreTargetUnavailable,
    QueryLimitExceeded,
};

ProtocolError protocolError(ErrorCode code);
ProtocolError protocolError(SubmitError error);

// 流式发送时 headers 不含 body，长度取 body.size()；提前拒绝可显式转为缓冲响应。
struct JsonResponse {
    muduox::net::HttpResponse headers{ false };
    std::string body;
    // HEAD 上传成功不声明表示长度；错误仍使用实际 JSON 长度。
    bool omitLength = false;
    muduox::net::HttpResponse buffered(bool close) const;
};

JsonResponse errorResponse(ProtocolError error, RequestId requestId);
Result<JsonResponse> createdDirectoryResponse(const CreateDirectoryResult& result, RequestId requestId);
JsonResponse healthResponse(bool ready, RequestId requestId);
JsonResponse liveResponse(RequestId requestId);
Result<std::string> formatUtcTimestamp(UtcTime time);

}  // namespace anof::http
