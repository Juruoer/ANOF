#include <anof/client/DriveClient.h>
#include <anof/client/internal/Json.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <anof/domain/drive/TreePolicy.h>
namespace anof::client {
namespace {
using namespace internal;
SecureBuffer target(std::optional<NodeId> parent, const std::optional<std::string>& name) {
    SecureBuffer b(kRequestLimit);
    append(b, "{");
    if (parent) {
        append(b, "\"parent_id\":");
        quoted(b, parent->toString());
    }
    if (name) {
        if (parent) append(b, ",");
        append(b, "\"name\":");
        quoted(b, *name);
    }
    append(b, "}");
    return b;
}
std::string encode(std::string_view s) {
    std::string out;
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
ClientError mismatch(const ResponseMeta& meta, bool write = true) {
    auto e = failure(write ? ErrorKind::OutcomeUnknown : ErrorKind::Protocol);
    e.httpStatus = meta.httpStatus;
    e.requestId = meta.requestId;
    return e;
}
bool valid(const ConditionTag& t) { return bool(driveCondition(t.value())); }
}  // namespace
template <class T>
ClientResult<Reply<T>> DriveClient::call(DriveOperation op, Method method, std::string path, SecureBuffer body, std::optional<ConditionTag> tag, unsigned limit) {
    if (!profile_.usable()) return failure(ErrorKind::LocalState, "PROFILE_RELOAD_REQUIRED");
    if (!profile_.profile().credential) return failure(ErrorKind::AuthenticationRequired);
    if (cancel_ && cancel_->load()) return failure(ErrorKind::Interrupted);
    bool started = false;
    try {
        HttpRequest request{ method, "/api/v1" + path, std::move(body), &profile_.profile().credential->accessToken, std::move(tag) };
        started = true;
        auto response = transport_.perform(profile_.profile().server, request, cancel_);
        if (!response) return response.error();
        auto decoded = decodeDriveResponse(op, response.value(), limit);
        if (!decoded) return decoded.error();
        return Reply<T>{ std::get<T>(std::move(decoded.value().data)), std::move(decoded.value().meta) };
    } catch (...) {
        auto error = failure(started ? (method != Method::Get ? ErrorKind::OutcomeUnknown : ErrorKind::Transport) : ErrorKind::LocalState);
        if (started) error.remote = RemoteOutcome::Unknown;
        return error;
    }
}
ClientResult<Reply<Node>> DriveClient::update(NodeId id, std::optional<NodeId> parent, std::optional<std::string> name, const ConditionTag& tag) {
    if ((!parent && !name) || (name && !validateDirectoryName(*name)) || !valid(tag)) return failure(ErrorKind::InvalidArgument);
    auto r = call<Node>(DriveOperation::Update, Method::Patch, "/nodes/" + id.toString(), target(parent, name), tag);
    if (r && (r.value().data.id != id || (parent && r.value().data.parentId != parent) || (name && r.value().data.name != *name))) return mismatch(r.value().meta);
    return r;
}
ClientResult<Reply<Node>> DriveClient::copy(NodeId id, SpaceId space, NodeId parent, std::string name, const ConditionTag& tag) {
    if (!validateDirectoryName(name) || !valid(tag)) return failure(ErrorKind::InvalidArgument);
    SecureBuffer b(kRequestLimit);
    append(b, "{\"space_id\":");
    quoted(b, space.toString());
    append(b, ",\"parent_id\":");
    quoted(b, parent.toString());
    append(b, ",\"name\":");
    quoted(b, name);
    append(b, "}");
    auto r = call<Node>(DriveOperation::Copy, Method::Post, "/nodes/" + id.toString() + "/copy", std::move(b), tag);
    if (r && (r.value().data.id == id || r.value().data.spaceId != space || r.value().data.parentId != parent || r.value().data.name != name)) return mismatch(r.value().meta);
    return r;
}
ClientResult<Reply<Trash>> DriveClient::remove(NodeId id, const ConditionTag& tag) {
    if (!valid(tag)) return failure(ErrorKind::InvalidArgument);
    auto r = call<Trash>(DriveOperation::Remove, Method::Delete, "/nodes/" + id.toString(), {}, tag);
    if (r && r.value().data.rootNodeId != id) return mismatch(r.value().meta);
    return r;
}
ClientResult<Reply<Page<Node>>> DriveClient::search(SpaceId space, std::string query, PageRequest page) {
    if (!SearchText::parse(query)) return failure(ErrorKind::InvalidArgument);
    auto q = pageQuery(page);
    if (!q) return q.error();
    auto r = call<Page<Node>>(DriveOperation::Search, Method::Get, "/search" + q.value() + "&space_id=" + space.toString() + "&q=" + encode(query), {}, {}, page.limit);
    if (r)
        for (const auto& n : r.value().data.items)
            if (n.spaceId != space || n.name.find(query) == std::string::npos) return mismatch(r.value().meta, false);
    return r;
}
ClientResult<Reply<Page<Trash>>> DriveClient::listTrash(SpaceId space, std::optional<NodeId> root, PageRequest page) {
    auto q = pageQuery(page);
    if (!q) return q.error();
    auto r = call<Page<Trash>>(DriveOperation::ListTrash, Method::Get, "/trash" + q.value() + "&space_id=" + space.toString() + (root ? "&root_node_id=" + root->toString() : ""), {}, {}, page.limit);
    if (r)
        for (const auto& t : r.value().data.items)
            if (t.spaceId != space || (root && t.rootNodeId != *root)) return mismatch(r.value().meta, false);
    return r;
}
ClientResult<Reply<Trash>> DriveClient::showTrash(TrashId id) {
    auto r = call<Trash>(DriveOperation::ShowTrash, Method::Get, "/trash/" + id.toString());
    if (r && r.value().data.id != id) return mismatch(r.value().meta, false);
    return r;
}
ClientResult<Reply<Node>> DriveClient::restore(TrashId id, std::optional<NodeId> parent, std::optional<std::string> name, const ConditionTag& tag) {
    if ((name && !validateDirectoryName(*name)) || !valid(tag)) return failure(ErrorKind::InvalidArgument);
    auto r = call<Node>(DriveOperation::Restore, Method::Post, "/trash/" + id.toString() + "/restore", target(parent, name), tag);
    if (r && ((parent && r.value().data.parentId != parent) || (name && r.value().data.name != *name))) return mismatch(r.value().meta);
    return r;
}
ClientResult<Reply<Done>> DriveClient::purge(TrashId id, const ConditionTag& tag) {
    if (!valid(tag)) return failure(ErrorKind::InvalidArgument);
    return call<Done>(DriveOperation::Purge, Method::Delete, "/trash/" + id.toString(), {}, tag);
}
}  // namespace anof::client
