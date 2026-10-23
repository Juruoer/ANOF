#include <anof/client/TransferProtocol.h>
#include <anof/domain/drive/NodeAccounting.h>
#include <anof/client/internal/Json.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <anof/domain/transfer/TransferValues.h>
namespace anof::client {
namespace {
using namespace internal;
std::string header(const HttpResponse& r, const char* name) {
    auto it = r.headers.find(name);
    require(it != r.headers.end());
    return it->second;
}
bool writes(TransferOperation op) {
    return op == TransferOperation::Create || op == TransferOperation::Complete || op == TransferOperation::Cancel ||
           op == TransferOperation::SetStorage || op == TransferOperation::Patch;
}
Json upload(const Json& j) {
    Json out;
    for (auto key : { "id", "instance_id", "client_upload_id", "space_id", "parent_id" })
        out[key] = id<NodeId>(field(j, key)).toString();
    const auto name = string(j, "name");
    require(bool(validateDirectoryName(name)));
    out["name"] = name;
    const auto length = decimal(string(j, "length")), offset = decimal(string(j, "offset"));
    require(length <= kMaxFileBytes && offset <= length);
    out["length"] = std::to_string(length);
    out["offset"] = std::to_string(offset);
    for (auto key : { "expected_sha256", "actual_sha256" }) {
        out[key] = nullptr;
        if (!field(j, key).is_null()) {
            require(bool(Sha256::parse(string(j, key))));
            out[key] = string(j, key);
        }
    }
    auto state = string(j, "state");
    require(std::set<std::string>{ "CREATING", "OPEN", "VERIFYING", "READY", "COMMITTING", "COMPLETED", "CANCELLED",
                                   "EXPIRED", "FAILED" }
                .count(state));
    out["state"] = state;
    if (state == "READY" || state == "COMMITTING" || state == "COMPLETED")
        require(offset == length && !out["actual_sha256"].is_null());
    if (state == "COMPLETED" && !out["expected_sha256"].is_null())
        require(out["actual_sha256"] == out["expected_sha256"]);
    out["result_node_id"] = nullptr;
    if (!field(j, "result_node_id").is_null())
        out["result_node_id"] = id<NodeId>(field(j, "result_node_id")).toString();
    require((state == "COMPLETED") == !out["result_node_id"].is_null());
    for (auto key : { "created_at", "last_progress_at", "expires_at" }) out[key] = time(field(j, key));
    out["terminal_at"] = nullptr;
    if (!field(j, "terminal_at").is_null()) out["terminal_at"] = time(field(j, "terminal_at"));
    require((state == "COMPLETED" || state == "CANCELLED" || state == "EXPIRED" || state == "FAILED") ==
            !out["terminal_at"].is_null());
    require(out["created_at"] <= out["last_progress_at"] && out["created_at"] < out["expires_at"]);
    if(!out["terminal_at"].is_null()) require(out["terminal_at"] >= out["last_progress_at"]);
    out["result_deleted_at"] = nullptr;
    if(!field(j,"result_deleted_at").is_null()) {
        out["result_deleted_at"] = time(field(j,"result_deleted_at"));
        require(state=="COMPLETED" && out["result_deleted_at"] >= out["terminal_at"]);
    }
    out["last_error"] = nullptr;
    if (!field(j, "last_error").is_null()) {
        auto e = string(j, "last_error");
        require(std::set<std::string>{ "NAME_CONFLICT", "PARENT_NOT_DIRECTORY", "DEPTH_LIMIT_EXCEEDED", "NOT_FOUND",
                                       "CHECKSUM_MISMATCH", "UPLOAD_CANCELLED", "UPLOAD_EXPIRED", "USER_DISABLED",
                                       "STORAGE_FULL", "TEMPORARILY_UNAVAILABLE", "INTERNAL_ERROR",
                                       "COMMIT_OUTCOME_UNKNOWN" }
                    .count(e));
        out["last_error"] = e;
    }
    out["max_patch_bytes"] = integer(field(j, "max_patch_bytes"), 1, kMaxPatchBytes);
    return out;
}
}  // namespace
ClientResult<TransferReply> decodeTransfer(TransferOperation op, const HttpResponse& r, unsigned limit) {
    try {
        if((op==TransferOperation::UploadHead||op==TransferOperation::ContentHead)&&r.status>=400&&r.status<=599) {
            auto error=failure(ErrorKind::Application,"HTTP_REJECTED");error.httpStatus=r.status;error.remote=RemoteOutcome::Rejected;
            if(auto it=r.headers.find("x-request-id");it!=r.headers.end()) {
                auto parsed=RequestId::parse(it->second);if(parsed&&parsed.value().toString()==it->second)error.requestId=parsed.value();
            }
            return error;
        }
        auto rid = RequestId::parse(header(r, "x-request-id"));
        require(bool(rid) && rid.value().toString()==header(r,"x-request-id"));
        ResponseMeta meta{ r.status, rid.value(), {} };
        if (r.headers.count("etag")) meta.etag = header(r, "etag");
        require(!r.headers.count("content-encoding") || header(r, "content-encoding") == "identity");
        const bool head = op == TransferOperation::UploadHead || op == TransferOperation::ContentHead;
        if (r.status >= 400) {
            ClientError error = failure(ErrorKind::Application, "HTTP_REJECTED");
            error.httpStatus = r.status;
            error.requestId = rid.value();
            error.remote = RemoteOutcome::Rejected;
            if (head) return error;
            Document doc;
            doc.parse(r.body.view());
            const auto& e = field(doc.root, "error");
            auto code = string(e, "code");
            static const std::map<std::string, int> codes{ { "INVALID_ARGUMENT", 400 },
                                                           { "INVALID_NAME", 400 },
                                                           { "REQUEST_TOO_LARGE", 413 },
                                                           { "LENGTH_REQUIRED", 411 },
                                                           { "UNSUPPORTED_MEDIA_TYPE", 415 },
                                                           { "CONCURRENT_MODIFICATION", 409 },
                                                           { "UNAUTHENTICATED", 401 },
                                                           { "FORBIDDEN", 403 },
                                                           { "NOT_FOUND", 404 },
                                                           { "PRECONDITION_FAILED", 412 },
                                                           { "PRECONDITION_REQUIRED", 428 },
                                                           { "RATE_LIMITED", 429 },
                                                           { "INTERNAL_ERROR", 500 },
                                                           { "COMMIT_OUTCOME_UNKNOWN", 500 },
                                                           { "TEMPORARILY_UNAVAILABLE", 503 },
                                                           { "STORAGE_FULL", 503 },
                                                           { "NAME_CONFLICT", 409 },
                                                           { "PARENT_NOT_DIRECTORY", 409 },
                                                           { "DEPTH_LIMIT_EXCEEDED", 409 },
                                                           { "QUOTA_EXCEEDED", 403 },
                                                           { "NOT_A_FILE", 409 },
                                                           { "NODE_LIMIT_REACHED", 409 },
                                                           { "UPLOAD_KEY_CONFLICT", 409 },
                                                           { "UPLOAD_OFFSET_MISMATCH", 409 },
                                                           { "UPLOAD_LENGTH_MISMATCH", 409 },
                                                           { "UPLOAD_BUSY", 409 },
                                                           { "UPLOAD_INCOMPLETE", 409 },
                                                           { "UPLOAD_STATE_CONFLICT", 409 },
                                                           { "UPLOAD_COMMIT_IN_PROGRESS", 409 },
                                                           { "STORAGE_BUDGET_EXCEEDED", 409 },
                                                           { "UPLOAD_TERMINATED", 410 },
                                                           { "UPLOAD_LIMIT_REACHED", 429 } };
            auto known = codes.find(code);
            require(known != codes.end() && known->second == r.status &&
                    id<RequestId>(field(e, "request_id")) == rid.value());
            error.code = code;
            if (code == "COMMIT_OUTCOME_UNKNOWN") {
                error.kind = ErrorKind::OutcomeUnknown;
                error.remote = RemoteOutcome::Unknown;
            }
            return error;
        }
        require(!meta.etag || op==TransferOperation::Storage || op==TransferOperation::SetStorage || op==TransferOperation::ContentHead);
        Json data = Json::object();
        if (head) {
            require(r.status == 200 && r.body.view().empty());
            if (op == TransferOperation::ContentHead) {
                auto length = decimal(header(r, "content-length"));
                require(length <= kMaxFileBytes && meta.etag && meta.etag->size() == 73 &&
                        meta.etag->substr(0, 8) == "\"sha256-" && meta.etag->back() == '"');
                auto hash = meta.etag->substr(8, 64);
                require(bool(Sha256::parse(hash)));
                require(header(r, "content-type") == "application/octet-stream");
                data = { { "length", std::to_string(length) }, { "sha256", hash }, { "etag", *meta.etag } };
            } else {
                auto length = decimal(header(r, "upload-length")), offset = decimal(header(r, "upload-offset"));
                require(length <= kMaxFileBytes && offset <= length);
                auto state = header(r, "upload-state");
                require(std::set<std::string>{ "CREATING", "OPEN", "VERIFYING", "READY", "COMMITTING", "COMPLETED",
                                               "CANCELLED", "EXPIRED", "FAILED" }
                            .count(state));
                data = { { "length", std::to_string(length) },
                         { "offset", std::to_string(offset) },
                         { "state", state } };
            }
        } else if (op == TransferOperation::Cancel || op == TransferOperation::Patch) {
            require(r.status == 204 && r.body.view().empty());
            if (op == TransferOperation::Patch) data["offset"] = std::to_string(decimal(header(r, "upload-offset")));
        } else {
            auto type = header(r, "content-type");
            require(type == "application/json" || type == "application/json; charset=utf-8");
            require(r.status == 200 || (op == TransferOperation::Create && (r.status == 201 || r.status == 202)) ||
                    (op == TransferOperation::Complete && r.status == 202));
            Document doc;
            doc.parse(r.body.view());
            const auto& j = doc.root;
            if (op == TransferOperation::List) {
                const auto& items = field(j, "items");
                require(items.is_array() && items.size() <= limit && limit <= 200);
                data["items"] = Json::array();
                for (const auto& item : items) data["items"].push_back(upload(item));
                data["next_cursor"] = nullptr;
                if (!field(j, "next_cursor").is_null()) {
                    auto cursor = string(j, "next_cursor");
                    require(
                        !items.empty() && !cursor.empty() && cursor.size() <= 2048 &&
                        cursor.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
                            std::string::npos);
                    data["next_cursor"] = cursor;
                }
            } else if (op == TransferOperation::Storage || op == TransferOperation::SetStorage) {
                data["instance_id"] = id<InstanceId>(field(j, "instance_id")).toString();
                for (auto key : { "content_budget_bytes", "retained_bytes", "reserved_bytes" })
                    data[key] = std::to_string(decimal(string(j, key)));
                for(auto key:{"pending_gc_bytes","pending_gc_objects","deleting_bytes","deleting_objects","used_nodes","reserved_nodes","node_limit"})
                    data[key]=std::to_string(decimal(string(j,key)));
                const auto used=decimal(string(j,"used_nodes")), reserved=decimal(string(j,"reserved_nodes"));
                require(decimal(string(j,"node_limit"))==kInstanceNodeLimit && used<=kInstanceNodeLimit && reserved<=kInstanceNodeLimit-used);
                require(decimal(string(j,"deleting_bytes"))<=decimal(string(j,"pending_gc_bytes")) && decimal(string(j,"pending_gc_bytes"))<=decimal(string(j,"retained_bytes")));
                require(decimal(string(j,"deleting_objects"))<=decimal(string(j,"pending_gc_objects")));
                auto prefix = "\"storage-" + data["instance_id"].get<std::string>() + "-";
                require(meta.etag && meta.etag->compare(0, prefix.size(), prefix) == 0 && meta.etag->back() == '"');
                decimal(std::string_view(*meta.etag).substr(prefix.size(), meta.etag->size() - prefix.size() - 1), 1);
                auto disk = string(j, "disk_status");
                require(disk == "AVAILABLE" || disk == "UNAVAILABLE");
                data["disk_status"] = disk;
                for (auto key : { "free_bytes", "safety_margin_bytes" }) {
                    data[key] = nullptr;
                    if (disk == "AVAILABLE")
                        data[key] = std::to_string(decimal(string(j, key)));
                    else
                        require(field(j, key).is_null());
                }
                data["sampled_at"] = disk == "AVAILABLE" ? Json(time(field(j, "sampled_at"))) : Json(nullptr);
            } else {
                data = upload(j);
                if (r.status == 201)
                    require(header(r, "location") == "/api/v1/uploads/" + data["id"].get<std::string>());
            }
        }
        return TransferReply(std::move(data), std::move(meta));
    } catch (...) {
        auto e = failure(writes(op) ? ErrorKind::OutcomeUnknown : ErrorKind::Protocol);
        e.remote = writes(op) ? RemoteOutcome::Unknown : RemoteOutcome::NotSent;
        return e;
    }
}
}  // namespace anof::client
