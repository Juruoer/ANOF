#include <anof/transport/http/DriveResponse.h>
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/internal/JsonOutput.h>
#include <anof/transport/http/internal/NodeJson.h>
namespace anof::http {
namespace {
using Json = nlohmann::json;
std::string tag(const TrashEntry& e) { return "\"trash-" + e.id.toString() + "-" + std::to_string(e.revision.value()) + "\""; }
Result<Json> trashJson(const TrashView& view) {
    const auto& e = view.entry;
    if (!validateTrashEntry(e)) return ErrorCode::InternalError;
    auto deleted = formatUtcTimestamp(e.deletedAt), expires = formatUtcTimestamp(e.expiresAt);
    if (!deleted || !expires) return ErrorCode::InternalError;
    return Json{{"id", e.id.toString()}, {"instance_id", view.instanceId.toString()}, {"space_id", e.spaceId.toString()},
        {"root_node_id", e.rootNodeId.toString()}, {"original_parent_id", e.originalParentId.toString()}, {"name", e.originalName},
        {"kind", e.kind == NodeKind::File ? "file" : "directory"}, {"deleted_at", deleted.value()}, {"expires_at", expires.value()},
        {"revision", std::to_string(e.revision.value())}, {"etag", tag(e)}, {"node_count", std::to_string(e.nodeCount.value())}, {"logical_bytes", std::to_string(e.logicalBytes.value())}};
}
}
Result<JsonResponse> restoredNodeResponse(const NodeDetails& node, RequestId id) {
    auto value = nodeResponse(node, id);
    if (value) value.value().headers.addHeader("Location", "/api/v1/nodes/" + node.metadata.id.toString());
    return value;
}
Result<JsonResponse> copiedNodeResponse(const NodeDetails& node, RequestId id) {
    auto value = restoredNodeResponse(node, id);
    if (value) {
        value.value().headers.setStatusCode(muduox::net::HttpResponse::k201Created);
        value.value().headers.setStatusMessage("Created");
    }
    return value;
}
Result<JsonResponse> trashResponse(const TrashView& view, RequestId id) {
    auto json = trashJson(view);
    if (!json) return json.error();
    auto response = internal::jsonResponse(200, "OK", id, json.value(), kMaxIdentityResponseBytes);
    response.headers.addHeader("ETag", tag(view.entry));
    return response;
}
Result<JsonResponse> trashedNodeResponse(const TrashView& view, RequestId id) {
    auto response = trashResponse(view, id);
    if (response) response.value().headers.addHeader("Location", "/api/v1/trash/" + view.entry.id.toString());
    return response;
}
Result<JsonResponse> searchResponse(const Page<NodeDetails, SearchCursor>& page, RequestId id) {
    if (page.items.size() > 200 || (page.items.empty() && page.next)) return ErrorCode::InternalError;
    Json items = Json::array(), next = nullptr;
    for (const auto& item : page.items) {
        if (!item.metadata.parentId) return ErrorCode::InternalError;
        auto value = internal::nodeJson(item);
        if (!value) return value.error();
        items.push_back(std::move(value).value());
    }
    if (page.next) {
        const auto& last = page.items.back().metadata;
        if (last.id != page.next->position.id || last.name != page.next->position.name || last.spaceId != page.next->space) return ErrorCode::InternalError;
        auto cursor = encodeCursor(*page.next);
        if (!cursor) return cursor.error();
        next = cursor.value();
    }
    return internal::jsonResponse(200, "OK", id, {{"items", std::move(items)}, {"next_cursor", std::move(next)}}, kMaxIdentityResponseBytes);
}
Result<JsonResponse> trashPageResponse(const Page<TrashView, TrashCursor>& page, RequestId id) {
    if (page.items.size() > 200 || (page.items.empty() && page.next)) return ErrorCode::InternalError;
    Json items = Json::array(), next = nullptr;
    for (const auto& item : page.items) {
        auto value = trashJson(item);
        if (!value) return value.error();
        items.push_back(std::move(value).value());
    }
    if (page.next) {
        const auto& last = page.items.back().entry;
        if (last.id != page.next->position.id || last.deletedAt != page.next->position.deletedAt || last.spaceId != page.next->space ||
            (page.next->root && *page.next->root != last.rootNodeId)) return ErrorCode::InternalError;
        auto cursor = encodeCursor(*page.next);
        if (!cursor) return cursor.error();
        next = cursor.value();
    }
    return internal::jsonResponse(200, "OK", id, {{"items", std::move(items)}, {"next_cursor", std::move(next)}}, kMaxIdentityResponseBytes);
}
}  // namespace anof::http
