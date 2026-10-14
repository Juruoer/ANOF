#include <anof/transport/http/JsonResponse.h>
#include <anof/transport/http/internal/JsonOutput.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace anof::http {
namespace {
struct ErrorDescription { int status; const char* reason; const char* code; const char* message; };
ErrorDescription describe(ProtocolError error) {
    switch (error) {
    case ProtocolError::LoginConflict: return {409, "Conflict", "LOGIN_CONFLICT", "Login unavailable"};
    case ProtocolError::InviteUnavailable: return {409, "Conflict", "INVITE_UNAVAILABLE", "Invitation unavailable"};
    case ProtocolError::UserLimitReached: return {409, "Conflict", "USER_LIMIT_REACHED", "User limit reached"};
    case ProtocolError::Forbidden: return {403, "Forbidden", "FORBIDDEN", "Operation forbidden"};
    case ProtocolError::PreconditionFailed: return {412, "Precondition Failed", "PRECONDITION_FAILED", "Precondition failed"};
    case ProtocolError::PreconditionRequired: return {428, "Precondition Required", "PRECONDITION_REQUIRED", "Precondition required"};
    case ProtocolError::SessionLimitReached: return {409, "Conflict", "SESSION_LIMIT_REACHED", "Session limit reached"};

    case ProtocolError::InvalidArgument: return {400, "Bad Request", "INVALID_ARGUMENT", "Invalid request"};
    case ProtocolError::InvalidName: return {400, "Bad Request", "INVALID_NAME", "Invalid directory name"};
    case ProtocolError::Unauthenticated: return {401, "Unauthorized", "UNAUTHENTICATED", "Authentication required"};
    case ProtocolError::NotFound: return {404, "Not Found", "NOT_FOUND", "Resource unavailable"};
    case ProtocolError::ParentNotDirectory: return {409, "Conflict", "PARENT_NOT_DIRECTORY", "Parent is not a directory"};
    case ProtocolError::DepthLimitExceeded: return {409, "Conflict", "DEPTH_LIMIT_EXCEEDED", "Directory depth exceeded"};
    case ProtocolError::NameConflict: return {409, "Conflict", "NAME_CONFLICT", "Name already exists"};
    case ProtocolError::QuotaLimitExceeded: return {409, "Conflict", "QUOTA_EXCEEDED", "Quota limit below current usage"};
    case ProtocolError::QuotaExceeded: return {403, "Forbidden", "QUOTA_EXCEEDED", "Node quota exceeded"};
    case ProtocolError::ConcurrentModification: return {409, "Conflict", "CONCURRENT_MODIFICATION", "Concurrent modification"};
    case ProtocolError::RequestTooLarge: return {413, "Content Too Large", "REQUEST_TOO_LARGE", "Request body too large"};
    case ProtocolError::UnsupportedMediaType: return {415, "Unsupported Media Type", "UNSUPPORTED_MEDIA_TYPE", "Unsupported content type or encoding"};
    case ProtocolError::RateLimited: return {429, "Too Many Requests", "RATE_LIMITED", "Request rate exceeded"};
    case ProtocolError::CommitOutcomeUnknown: return {500, "Internal Server Error", "COMMIT_OUTCOME_UNKNOWN", "Commit outcome is unknown"};
    case ProtocolError::InternalError: return {500, "Internal Server Error", "INTERNAL_ERROR", "Internal error"};
    case ProtocolError::TemporarilyUnavailable: return {503, "Service Unavailable", "TEMPORARILY_UNAVAILABLE", "Service temporarily unavailable"};
    case ProtocolError::StorageFull: return {507, "Insufficient Storage", "STORAGE_FULL", "Storage full"};
    }
    throw std::invalid_argument("Unknown protocol error");
}
}  // namespace

JsonResponse internal::jsonResponse(int status, const char* reason, RequestId requestId, const nlohmann::json& body, std::size_t limit) {
    // 当前 muduox 无部分命名枚举；只允许本模块已审定的状态码进入转换。
    switch (status) {
    case 200: case 201: case 400: case 401: case 403: case 404: case 409:
    case 412: case 413: case 415: case 428: case 429: case 500: case 503: case 507: break;
    default: throw std::invalid_argument("Unsupported HTTP status");
    }
    JsonResponse result;
    result.headers.setStatusCode(static_cast<muduox::net::HttpResponse::HttpStatusCode>(status));
    result.headers.setStatusMessage(reason);
    result.headers.setContentType("application/json");
    result.headers.addHeader("Cache-Control", "no-store");
    result.headers.addHeader("X-Request-ID", requestId.toString());
    result.body = body.dump();
    if (result.body.size() > limit) throw std::length_error("JSON response exceeds limit");
    return result;
}

using internal::jsonResponse;

ProtocolError protocolError(ErrorCode code) {
    switch (code) {
    case ErrorCode::LoginConflict: return ProtocolError::LoginConflict;
    case ErrorCode::InviteUnavailable: return ProtocolError::InviteUnavailable;
    case ErrorCode::UserLimitReached: return ProtocolError::UserLimitReached;
    case ErrorCode::Forbidden: return ProtocolError::Forbidden;
    case ErrorCode::PreconditionFailed: return ProtocolError::PreconditionFailed;
    case ErrorCode::PreconditionRequired: return ProtocolError::PreconditionRequired;
    case ErrorCode::RateLimited: return ProtocolError::RateLimited;
    case ErrorCode::SessionLimitReached: return ProtocolError::SessionLimitReached;

    case ErrorCode::InvalidArgument: return ProtocolError::InvalidArgument;
    case ErrorCode::InvalidName: return ProtocolError::InvalidName;
    case ErrorCode::Unauthenticated: return ProtocolError::Unauthenticated;
    case ErrorCode::NotFound: return ProtocolError::NotFound;
    case ErrorCode::ParentNotDirectory: return ProtocolError::ParentNotDirectory;
    case ErrorCode::DepthLimitExceeded: return ProtocolError::DepthLimitExceeded;
    case ErrorCode::NameConflict: return ProtocolError::NameConflict;
    case ErrorCode::QuotaExceeded: return ProtocolError::QuotaExceeded;
    case ErrorCode::Conflict: return ProtocolError::ConcurrentModification;
    case ErrorCode::Retryable: return ProtocolError::TemporarilyUnavailable;
    case ErrorCode::StorageFull: return ProtocolError::StorageFull;
    case ErrorCode::CommitOutcomeUnknown: return ProtocolError::CommitOutcomeUnknown;
    case ErrorCode::IntegrityError: case ErrorCode::InternalError: return ProtocolError::InternalError;
    }
    return ProtocolError::InternalError;
}

ProtocolError protocolError(SubmitError error) {
    switch (error) {
    case SubmitError::RateLimited: return ProtocolError::RateLimited;
    case SubmitError::QueueFull: case SubmitError::Stopping: return ProtocolError::TemporarilyUnavailable;
    case SubmitError::InternalError: return ProtocolError::InternalError;
    }
    return ProtocolError::InternalError;
}

muduox::net::HttpResponse JsonResponse::buffered(bool close) const {
    auto result = headers;
    result.setCloseConnection(close);
    result.setBody(body);
    return result;
}

JsonResponse errorResponse(ProtocolError error, RequestId requestId) {
    const auto description = describe(error);
    auto result = jsonResponse(description.status, description.reason, requestId,
        {{"error", {{"code", description.code}, {"message", description.message},
                     {"request_id", requestId.toString()}, {"details", nlohmann::json::object()}}}});
    if (error == ProtocolError::Unauthenticated) result.headers.addHeader("WWW-Authenticate", "Bearer realm=\"anof\"");
    return result;
}

Result<std::string> formatUtcTimestamp(UtcTime time) {
    const auto micros = time.time_since_epoch().count();
    auto seconds = micros / 1000000;
    auto fraction = micros % 1000000;
    if (fraction < 0) { --seconds; fraction += 1000000; }
    // 先限定四位公历年份，避免 time_t 转换和格式输出越界；不乘回微秒以免溢出。
    if (seconds < -62135596800LL || seconds > 253402300799LL) return ErrorCode::InternalError;
    const auto raw = static_cast<std::time_t>(seconds);
    if (static_cast<decltype(seconds)>(raw) != seconds) return ErrorCode::InternalError;
    std::tm utc{};
    if (!gmtime_r(&raw, &utc)) return ErrorCode::InternalError;
    char buffer[28];
    const auto count = std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(fraction));
    if (count != 27) return ErrorCode::InternalError;
    return std::string(buffer, 27);
}

Result<JsonResponse> createdDirectoryResponse(const CreateDirectoryResult& result, RequestId requestId) {
    const auto& node = result.directory;
    // 返回成功之前验证线上表示不变量；表示失败不意味着已经提交的事务被撤销。
    if (!node.parentId || node.revision.value() != 1 || node.createdAt != node.updatedAt || !validateDirectoryName(node.name)) return ErrorCode::InternalError;
    const auto created = formatUtcTimestamp(node.createdAt);
    if (!created) return created.error();
    auto output = jsonResponse(201, "Created", requestId,
        {{"id", node.id.toString()}, {"space_id", node.spaceId.toString()}, {"parent_id", node.parentId->toString()},
         {"name", node.name}, {"kind", "directory"}, {"created_by", node.createdBy.toString()},
         {"created_at", created.value()}, {"updated_at", created.value()}});
    output.headers.addHeader("Location", "/api/v1/nodes/" + node.id.toString());
    output.headers.addHeader("ETag", "\"node-" + node.id.toString() + "-1\"");
    return output;
}
JsonResponse healthResponse(bool ready, RequestId requestId) {
    return ready ? jsonResponse(200, "OK", requestId, {{"status", "ready"}}) : errorResponse(ProtocolError::TemporarilyUnavailable, requestId);
}
JsonResponse liveResponse(RequestId requestId) { return jsonResponse(200, "OK", requestId, {{"status", "live"}}); }
}  // namespace anof::http
