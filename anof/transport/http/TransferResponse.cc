#include <anof/transport/http/TransferResponse.h>
#include <anof/transport/http/IdentityResponse.h>
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/internal/JsonOutput.h>
#include <anof/domain/drive/DirectoryPolicy.h>
namespace anof::http {
namespace {
using Json = nlohmann::json;
const char* stateName(UploadState state) {
    switch (state) {
    case UploadState::Creating: return "CREATING";
    case UploadState::Open: return "OPEN";
    case UploadState::Verifying: return "VERIFYING";
    case UploadState::Ready: return "READY";
    case UploadState::Committing: return "COMMITTING";
    case UploadState::Completed: return "COMPLETED";
    case UploadState::Cancelled: return "CANCELLED";
    case UploadState::Expired: return "EXPIRED";
    case UploadState::Failed: return "FAILED";
    }
    return nullptr;
}
const char* failureName(TransferFailure failure) {
    switch (failure) {
    case TransferFailure::NameConflict: return "NAME_CONFLICT";
    case TransferFailure::ParentNotDirectory: return "PARENT_NOT_DIRECTORY";
    case TransferFailure::DepthLimitExceeded: return "DEPTH_LIMIT_EXCEEDED";
    case TransferFailure::NotFound: return "NOT_FOUND";
    case TransferFailure::ChecksumMismatch: return "CHECKSUM_MISMATCH";
    case TransferFailure::UploadCancelled: return "UPLOAD_CANCELLED";
    case TransferFailure::UploadExpired: return "UPLOAD_EXPIRED";
    case TransferFailure::UserDisabled: return "USER_DISABLED";
    case TransferFailure::StorageFull: return "STORAGE_FULL";
    case TransferFailure::TemporarilyUnavailable: return "TEMPORARILY_UNAVAILABLE";
    case TransferFailure::InternalError: return "INTERNAL_ERROR";
    case TransferFailure::CommitOutcomeUnknown: return "COMMIT_OUTCOME_UNKNOWN";
    }
    return nullptr;
}
Result<Json> uploadJson(const UploadView& u) {
    auto created = formatUtcTimestamp(u.createdAt), progress = formatUtcTimestamp(u.lastProgressAt), expires = formatUtcTimestamp(u.expiresAt);
    const auto* state = stateName(u.state);
    const bool terminal = u.state == UploadState::Completed || u.state == UploadState::Cancelled || u.state == UploadState::Expired || u.state == UploadState::Failed;
    if (!state || !created || !progress || !expires || !validateDirectoryName(u.target.name) || u.offset.value() > u.length.value() ||
        terminal != u.terminalAt.has_value() || (u.state == UploadState::Completed) != u.resultNodeId.has_value() ||
        ((u.state == UploadState::Ready || u.state == UploadState::Committing || u.state == UploadState::Completed) && !u.actualSha256)) return ErrorCode::InternalError;
    Json time = nullptr, error = nullptr;
    if (u.terminalAt) { auto v = formatUtcTimestamp(*u.terminalAt); if (!v) return v.error(); time = v.value(); }
    if (u.lastError) { auto v = failureName(*u.lastError); if (!v) return ErrorCode::InternalError; error = v; }
    return Json{{"id", u.id.toString()}, {"instance_id", u.instanceId.toString()}, {"client_upload_id", u.clientUploadId.toString()},
        {"space_id", u.spaceId.toString()}, {"parent_id", u.target.parentId.toString()}, {"name", u.target.name},
        {"length", std::to_string(u.length.value())}, {"offset", std::to_string(u.offset.value())},
        {"expected_sha256", u.expectedSha256 ? Json(u.expectedSha256->toString()) : Json(nullptr)},
        {"actual_sha256", u.actualSha256 ? Json(u.actualSha256->toString()) : Json(nullptr)}, {"state", state},
        {"result_node_id", u.resultNodeId ? Json(u.resultNodeId->toString()) : Json(nullptr)}, {"created_at", created.value()},
        {"last_progress_at", progress.value()}, {"expires_at", expires.value()}, {"terminal_at", time}, {"last_error", error}, {"max_patch_bytes", kMaxPatchBytes}};
}
Result<JsonResponse> response(const UploadView& u, RequestId id, int status, const char* reason) {
    auto json = uploadJson(u);
    if (!json) return json.error();
    return internal::jsonResponse(status, reason, id, json.value(), kMaxIdentityResponseBytes);
}
}
ProtocolError transferError(ErrorCode error) {
    return error == ErrorCode::StorageFull ? ProtocolError::TransferStorageFull : protocolError(error);
}
Result<JsonResponse> uploadResponse(const UploadView& u, RequestId id) { return response(u, id, 200, "OK"); }
Result<JsonResponse> uploadHeadResponse(const UploadView& u, RequestId id) {
    if (!uploadJson(u)) return ErrorCode::InternalError;
    auto result = noContentResponse(id);
    result.headers.setStatusCode(muduox::net::HttpResponse::k200Ok);
    result.headers.setStatusMessage("OK");
    result.omitLength = true;
    result.headers.addHeader("Upload-Offset", std::to_string(u.offset.value()));
    result.headers.addHeader("Upload-Length", std::to_string(u.length.value()));
    result.headers.addHeader("Upload-State", stateName(u.state));
    result.headers.addHeader("Upload-Expires", formatUtcTimestamp(u.expiresAt).value());
    return result;
}
Result<JsonResponse> uploadCreatedResponse(const UploadCreated& value, RequestId id) {
    int status; const char* reason;
    switch (value.disposition) {
    case UploadCreateDisposition::Created: status = 201; reason = "Created"; break;
    case UploadCreateDisposition::Existing: status = 200; reason = "OK"; break;
    case UploadCreateDisposition::Creating: status = 202; reason = "Accepted"; break;
    default: return ErrorCode::InternalError;
    }
    auto result = response(value.upload, id, status, reason);
    if (result && status == 201) result.value().headers.addHeader("Location", "/api/v1/uploads/" + value.upload.id.toString());
    return result;
}
Result<JsonResponse> uploadCompleteResponse(const UploadCompletionAccepted& value, RequestId id) {
    switch (value.disposition) {
    case UploadCompleteDisposition::Accepted: return response(value.upload, id, 202, "Accepted");
    case UploadCompleteDisposition::Completed: return response(value.upload, id, 200, "OK");
    }
    return ErrorCode::InternalError;
}
Result<JsonResponse> uploadsResponse(const Page<UploadView, UploadCursor>& page, RequestId id) {
    if (page.items.size() > 200 || (page.items.empty() && page.next)) return ErrorCode::InternalError;
    Json items = Json::array(), next = nullptr;
    for (const auto& item : page.items) { auto value = uploadJson(item); if (!value) return value.error(); items.push_back(std::move(value).value()); }
    if (page.next) {
        if (page.next->position.id != page.items.back().id || page.next->position.createdAt != page.items.back().createdAt) return ErrorCode::InternalError;
        auto value = encodeCursor(*page.next); if (!value) return value.error(); next = value.value();
    }
    return internal::jsonResponse(200, "OK", id, {{"items", items}, {"next_cursor", next}}, kMaxIdentityResponseBytes);
}
Result<JsonResponse> storageResponse(const StorageView& s, RequestId id) {
    Json free = nullptr, margin = nullptr, sampled = nullptr;
    if (s.disk) {
        auto time = formatUtcTimestamp(s.disk->sampledAt); if (!time) return time.error();
        free = std::to_string(s.disk->free.value()); margin = std::to_string(s.disk->safetyMargin.value()); sampled = time.value();
    }
    auto result = internal::jsonResponse(200, "OK", id, {{"instance_id", s.instanceId.toString()}, {"content_budget_bytes", std::to_string(s.accounting.budget.value())},
        {"retained_bytes", std::to_string(s.accounting.retained.value())}, {"reserved_bytes", std::to_string(s.accounting.reserved.value())},
        {"disk_status", s.disk ? "AVAILABLE" : "UNAVAILABLE"}, {"free_bytes", free}, {"safety_margin_bytes", margin}, {"sampled_at", sampled}});
    result.headers.addHeader("ETag", "\"storage-" + s.instanceId.toString() + "-" + std::to_string(s.accounting.revision.value()) + "\"");
    return result;
}
JsonResponse patchResponse(UploadOffset offset, RequestId id) {
    auto result = noContentResponse(id); result.headers.addHeader("Upload-Offset", std::to_string(offset.value())); return result;
}
JsonResponse patchConflictResponse(PatchOffsetConflict conflict, RequestId id) {
    auto result = errorResponse(ProtocolError::UploadOffsetMismatch, id);
    auto json = Json::parse(result.body); json["error"]["details"] = {{"offset", std::to_string(conflict.offset.value())}}; result.body = json.dump(); return result;
}
}  // namespace anof::http
