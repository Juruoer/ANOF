#include <anof/transport/http/IdentityResponse.h>
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/internal/JsonOutput.h>
#include <anof/transport/http/internal/IdentityJson.h>
#include <anof/domain/drive/DirectoryPolicy.h>

namespace anof::http {
namespace {
using Json = nlohmann::json;
using internal::userTag;
using internal::userJson;
using internal::quotaJson;
std::string nodeTag(const DirectorySnapshot& node) {
    return "\"node-" + node.id.toString() + "-" + std::to_string(node.revision.value()) + "\"";
}
Result<Json> inviteJson(const InviteInfo& invite) {
    auto created = formatUtcTimestamp(invite.createdAt);
    auto expires = formatUtcTimestamp(invite.expiresAt);
    if (!created || !expires || invite.usedCount.value() > invite.maxUses.value()) return ErrorCode::InternalError;
    Json revoked = nullptr;
    if (invite.revokedAt) {
        auto time = formatUtcTimestamp(*invite.revokedAt);
        if (!time) return ErrorCode::InternalError;
        revoked = time.value();
    }
    return Json{{"id", invite.id.toString()}, {"created_by", invite.createdBy.toString()}, {"created_at", created.value()}, {"expires_at", expires.value()}, {"max_uses", invite.maxUses.value()}, {"used_count", invite.usedCount.value()}, {"revoked_at", std::move(revoked)}};
}
Result<Json> nodeJson(const NodeDetails& node) {
    const auto& metadata = node.metadata;
    const auto* file = std::get_if<FileContentMetadata>(&node.content);
    if (metadata.parentId ? !validateDirectoryName(metadata.name) : (!metadata.name.empty() || file)) return ErrorCode::InternalError;
    auto created = formatUtcTimestamp(metadata.createdAt);
    auto updated = formatUtcTimestamp(metadata.updatedAt);
    if (!created || !updated) return ErrorCode::InternalError;
    Json json{{"id", metadata.id.toString()}, {"space_id", metadata.spaceId.toString()}, {"parent_id", metadata.parentId ? Json(metadata.parentId->toString()) : Json(nullptr)}, {"name", metadata.name}, {"created_by", metadata.createdBy.toString()}, {"created_at", created.value()}, {"updated_at", updated.value()}, {"etag", nodeTag(metadata)}, {"kind", file ? "file" : "directory"}};
    if (file) {
        constexpr char digits[] = "0123456789abcdef";
        std::string hash;
        hash.reserve(64);
        for (const auto byte : file->sha256) {
            hash += digits[byte >> 4];
            hash += digits[byte & 15];
        }
        json["size"] = std::to_string(file->size.value());
        json["sha256"] = std::move(hash);
    }
    return json;
}
JsonResponse response(int status, RequestId id, const Json& json) {
    return internal::jsonResponse(status, status == 201 ? "Created" : "OK", id, json, kMaxIdentityResponseBytes);
}
// token 已按领域类型验证为 ASCII base64url（邀请带固定前缀），无需 JSON 转义。
// 所有非秘密字段先完成校验/序列化，预留容量后直接追加，避免把 token 放入 DOM。
void appendSecret(JsonResponse& output, std::string_view key, std::string_view token) {
    output.body.reserve(output.body.size() + key.size() + token.size() + 6);
    output.body.pop_back();
    output.body += ",\"";
    output.body.append(key);
    output.body += "\":\"";
    output.body.append(token);
    output.body += "\"}";
}
}  // namespace

Result<JsonResponse> registeredUserResponse(const UserProfile& user, RequestId id) {
    auto json = userJson(user);
    if (!json) return json.error();
    return response(201, id, json.value());
}
Result<JsonResponse> currentUserResponse(const UserProfile& user, RequestId id) {
    auto json = userJson(user);
    if (!json) return json.error();
    auto output = response(200, id, json.value());
    output.headers.addHeader("ETag", userTag(user));
    return output;
}
Result<JsonResponse> loginResponse(LoginResult result, RequestId id) {
    auto user = userJson(result.user);
    auto expires = formatUtcTimestamp(result.expiresAt);
    if (!user || !expires || !SecretToken::session(result.accessToken.view())) return ErrorCode::InternalError;
    auto output = response(201, id, {{"user", user.value()}, {"expires_at", expires.value()}, {"token_type", "Bearer"}});
    appendSecret(output, "access_token", result.accessToken.view());
    return output;
}
Result<JsonResponse> createdInviteResponse(CreatedInvite result, RequestId id) {
    auto invite = inviteJson(result.invite);
    if (!invite || !SecretToken::invite(result.token.view())) return ErrorCode::InternalError;
    auto output = response(201, id, {{"invite", invite.value()}});
    appendSecret(output, "token", result.token.view());
    return output;
}
JsonResponse noContentResponse(RequestId id) {
    JsonResponse output;
    output.headers.setStatusCode(static_cast<muduox::net::HttpResponse::HttpStatusCode>(204));
    output.headers.setStatusMessage("No Content");
    output.headers.addHeader("Cache-Control", "no-store");
    output.headers.addHeader("X-Request-ID", id.toString());
    return output;
}
Result<JsonResponse> spacesResponse(const std::vector<SpaceView>& spaces, RequestId id) {
    if (spaces.size() != 2 || spaces[0].space.id == spaces[1].space.id || spaces[0].space.kind == spaces[1].space.kind) return ErrorCode::InternalError;
    const SpaceView* ordered[] = {&spaces[0], &spaces[1]};
    if (ordered[1]->space.id < ordered[0]->space.id) std::swap(ordered[0], ordered[1]);
    Json items = Json::array();
    for (const auto* item : ordered) {
        const auto& space = item->space;
        const char* kind = nullptr;
        switch (space.kind) {
        case SpaceKind::Private:
            if (!space.ownerId || item->myContribution) return ErrorCode::InternalError;
            kind = "private";
            break;
        case SpaceKind::Public:
            if (space.ownerId || !item->myContribution) return ErrorCode::InternalError;
            kind = "public";
            break;
        }
        if (!kind) return ErrorCode::InternalError;
        items.push_back({{"id", space.id.toString()}, {"kind", kind}, {"owner_id", space.ownerId ? Json(space.ownerId->toString()) : Json(nullptr)}, {"root_node_id", space.rootNodeId.toString()}, {"quota", quotaJson(item->quota)}, {"my_contribution_quota", item->myContribution ? quotaJson(*item->myContribution) : Json(nullptr)}});
    }
    return response(200, id, {{"items", std::move(items)}});
}
Result<JsonResponse> nodeResponse(const NodeDetails& node, RequestId id) {
    auto json = nodeJson(node);
    if (!json) return json.error();
    auto output = response(200, id, json.value());
    output.headers.addHeader("ETag", nodeTag(node.metadata));
    return output;
}
Result<JsonResponse> childrenResponse(const Page<NodeDetails, ChildrenCursor>& page, RequestId id) {
    if (page.items.size() > 200 || (page.items.empty() && page.next)) return ErrorCode::InternalError;
    Json items = Json::array();
    for (const auto& item : page.items) {
        if (!item.metadata.parentId) return ErrorCode::InternalError;
        auto json = nodeJson(item);
        if (!json) return json.error();
        items.push_back(std::move(json).value());
    }
    Json next = nullptr;
    if (page.next) {
        const auto& last = page.items.back().metadata;
        if (page.next->position.id != last.id || page.next->position.name != last.name || page.next->spaceId != last.spaceId || page.next->parent != *last.parentId) return ErrorCode::InternalError;
        auto encoded = encodeCursor(*page.next);
        if (!encoded) return encoded.error();
        next = encoded.value();
    }
    return response(200, id, {{"items", std::move(items)}, {"next_cursor", std::move(next)}});
}
Result<JsonResponse> invitesResponse(const Page<InviteInfo, InviteCursor>& page, RequestId id) {
    if (page.items.size() > 200 || (page.items.empty() && page.next)) return ErrorCode::InternalError;
    Json items = Json::array();
    for (const auto& item : page.items) {
        auto json = inviteJson(item);
        if (!json) return json.error();
        items.push_back(std::move(json).value());
    }
    Json next = nullptr;
    if (page.next) {
        const auto& last = page.items.back();
        if (page.next->position.id != last.id || page.next->position.createdAt != last.createdAt) return ErrorCode::InternalError;
        auto encoded = encodeCursor(*page.next);
        if (!encoded) return encoded.error();
        next = encoded.value();
    }
    return response(200, id, {{"items", std::move(items)}, {"next_cursor", std::move(next)}});
}
}  // namespace anof::http
