#include <anof/client/Protocol.h>
#include <anof/client/internal/Json.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <algorithm>
#include <map>
namespace anof::client {
namespace {
using namespace internal;
bool writes(Operation op) { return op != Operation::Me && op != Operation::ListInvites && op != Operation::Spaces && op != Operation::Children && op != Operation::Stat && op != Operation::ListUsers && op != Operation::GetUser && op != Operation::GetQuota; }
int expected(Operation op) {
    switch (op) {
    case Operation::Register: case Operation::Login: case Operation::CreateInvite: case Operation::Mkdir: return 201;
    case Operation::Logout: case Operation::RevokeInvite: case Operation::ChangePassword: return 204;
    default: return 200;
    }
}
std::string header(const HttpResponse& r, const char* key) { auto found = r.headers.find(key); require(found != r.headers.end()); return found->second; }
std::string lower(std::string text) { for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A'); return text; }
bool jsonType(std::string value) {
    value = lower(std::move(value));
    const auto split = value.find(';');
    auto trim = [](std::string_view s) { while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1); while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1); return s; };
    if (trim(std::string_view(value).substr(0, split)) != "application/json") return false;
    if (split == std::string::npos) return true;
    const auto parameter = trim(std::string_view(value).substr(split + 1));
    return parameter == "charset=utf-8" || parameter == "charset=\"utf-8\"";
}
User user(const Json& j) {
    auto display = DisplayName::parse(string(j, "display_name")); require(static_cast<bool>(display));
    const auto& role = string(j, "role"); require(role == "member" || role == "administrator");
    User result{id<UserId>(field(j, "id")), login(field(j, "login")), std::move(display).value(), role == "member" ? User::Role::Member : User::Role::Admin,
        boolean(j, "enabled"), Revision::from(decimal(string(j, "revision"), 1)).value(), time(field(j, "created_at")), time(field(j, "updated_at"))};
    require(result.createdAt <= result.updatedAt); return result;
}
Invite invite(const Json& j) {
    Invite result{id<InviteId>(field(j, "id")), id<UserId>(field(j, "created_by")), time(field(j, "created_at")), time(field(j, "expires_at")),
        InviteUseLimit::from(integer(field(j, "max_uses"), 1, 10000)).value(), InviteUseCount::from(integer(field(j, "used_count"), 0, 10000)).value(), {}};
    require(result.createdAt < result.expiresAt && result.usedCount.value() <= result.maxUses.value());
    if (!field(j, "revoked_at").is_null()) { result.revokedAt = time(field(j, "revoked_at")); require(*result.revokedAt >= result.createdAt); }
    return result;
}
Amount amount(const Json& j) { return {ByteCount::from(decimal(string(j, "bytes"))).value(), NodeCount::from(decimal(string(j, "nodes"))).value()}; }
Quota quota(const Json& j) { return {amount(field(j, "limit")), amount(field(j, "used")), amount(field(j, "reserved")), Revision::from(decimal(string(j, "revision"), 1)).value()}; }
Space space(const Json& j) {
    const auto& kind = string(j, "kind"); require(kind == "private" || kind == "public");
    Space result{id<SpaceId>(field(j, "id")), kind == "private" ? Space::Kind::Private : Space::Kind::Public, {}, id<NodeId>(field(j, "root_node_id")), quota(field(j, "quota")), {}};
    if (!field(j, "owner_id").is_null()) result.ownerId = id<UserId>(field(j, "owner_id"));
    if (!field(j, "my_contribution_quota").is_null()) result.myContributionQuota = quota(field(j, "my_contribution_quota"));
    require(kind == "private" ? result.ownerId.has_value() && !result.myContributionQuota : !result.ownerId && result.myContributionQuota.has_value());
    return result;
}
void nodeTag(const std::string& tag, NodeId value) {
    const auto prefix = "\"node-" + value.toString() + "-";
    require(tag.size() > prefix.size() + 1 && tag.compare(0, prefix.size(), prefix) == 0 && tag.back() == '"');
    decimal(std::string_view(tag).substr(prefix.size(), tag.size() - prefix.size() - 1), 1);
}
Node node(const Json& j, std::optional<std::string> createdTag = {}) {
    Node result{id<NodeId>(field(j, "id")), id<SpaceId>(field(j, "space_id")), {}, string(j, "name"), id<UserId>(field(j, "created_by")),
        time(field(j, "created_at")), time(field(j, "updated_at")), createdTag ? *createdTag : string(j, "etag"), {}};
    if (!field(j, "parent_id").is_null()) result.parentId = id<NodeId>(field(j, "parent_id"));
    require(result.createdAt <= result.updatedAt);
    const auto& kind = string(j, "kind"); require(kind == "directory" || kind == "file");
    require(result.parentId ? static_cast<bool>(validateDirectoryName(result.name)) : result.name.empty() && kind == "directory");
    nodeTag(result.etag, result.id);
    if (kind == "file") {
        const auto& hash = string(j, "sha256"); require(hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos);
        result.file.emplace(FileContent{ByteCount::from(decimal(string(j, "size"))).value(), hash});
    }
    if (createdTag) require(kind == "directory" && result.parentId && result.createdAt == result.updatedAt && result.etag == "\"node-" + result.id.toString() + "-1\"");
    return result;
}
template<class T, class Parse> Page<T> page(const Json& j, unsigned limit, Parse parse) {
    const auto& items = field(j, "items"); require(items.is_array() && items.size() <= limit && items.size() <= 200);
    Page<T> result;
    for (const auto& item : items) result.items.push_back(parse(item));
    if (!field(j, "next_cursor").is_null()) {
        auto cursor = string(j, "next_cursor");
        require(!items.empty() && !cursor.empty() && cursor.size() <= 2048 && cursor.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos);
        result.nextCursor = std::move(cursor);
    }
    return result;
}
bool validError(Operation op, const std::string& code, int status) {
    static const std::map<std::string, int> common{{"REQUEST_ORIGIN_REJECTED",403},{"CSRF_REQUIRED",403},{"COOKIE_MODE_DISABLED",403},{"INVALID_ARGUMENT",400},{"RATE_LIMITED",429},{"INTERNAL_ERROR",500},{"TEMPORARILY_UNAVAILABLE",503}};
    if (auto it = common.find(code); it != common.end()) return it->second == status;
    if (code == "UNAUTHENTICATED") return status == 401 && op != Operation::Register;
    const bool accountWrite = op == Operation::Rename || op == Operation::ChangePassword || op == Operation::SetUserEnabled || op == Operation::SetQuotaLimit;
    const bool account = accountWrite || op == Operation::ListUsers || op == Operation::GetUser || op == Operation::GetQuota;
    if (account && code == "FORBIDDEN") return status == 403;
    if ((op == Operation::GetUser || op == Operation::SetUserEnabled || op == Operation::GetQuota || op == Operation::SetQuotaLimit) && code == "NOT_FOUND") return status == 404;
    if (accountWrite && code == "PRECONDITION_FAILED") return status == 412;
    if (accountWrite && code == "PRECONDITION_REQUIRED") return status == 428;
    if (op == Operation::SetQuotaLimit && code == "QUOTA_EXCEEDED") return status == 409;
    const bool post = accountWrite || op == Operation::Register || op == Operation::Login || op == Operation::CreateInvite || op == Operation::Mkdir;
    if (code == "UNSUPPORTED_MEDIA_TYPE") return status == 415 && post;
    if (code == "REQUEST_TOO_LARGE") return status == 413 && post;
    if (code == "COMMIT_OUTCOME_UNKNOWN") return status == 500 && writes(op);
    if (code == "STORAGE_FULL") return status == 507 && writes(op);
    if (code == "CONCURRENT_MODIFICATION") return status == 409 && (op == Operation::Register || op == Operation::Login || op == Operation::Mkdir);
    if (code == "NODE_LIMIT_REACHED" && (op == Operation::Register || op == Operation::Mkdir)) return status == 409;
    if (op == Operation::Register) return status == 409 && (code == "LOGIN_CONFLICT" || code == "INVITE_UNAVAILABLE" || code == "USER_LIMIT_REACHED");
    if (op == Operation::Login) return status == 409 && code == "SESSION_LIMIT_REACHED";
    if (op == Operation::CreateInvite || op == Operation::ListInvites || op == Operation::RevokeInvite) {
        if (code == "FORBIDDEN") return status == 403;
        return op == Operation::RevokeInvite && code == "NOT_FOUND" && status == 404;
    }
    if (op == Operation::Stat || op == Operation::Children || op == Operation::Mkdir) {
        if (code == "NOT_FOUND") return status == 404;
        if (code == "PARENT_NOT_DIRECTORY") return status == 409 && op != Operation::Stat && op != Operation::ListUsers && op != Operation::GetUser && op != Operation::GetQuota;
    }
    if (op == Operation::Mkdir) {
        if (code == "INVALID_NAME") return status == 400;
        if (code == "QUOTA_EXCEEDED") return status == 403;
        if (code == "NAME_CONFLICT" || code == "DEPTH_LIMIT_EXCEEDED") return status == 409;
    }
    return false;
}
}  // namespace
namespace internal {
Node parseNode(const Json& j) { return node(j); }
bool jsonContentType(std::string value) { return jsonType(std::move(value)); }
}
ClientResult<Reply<Payload>> decodeResponse(Operation op, const HttpResponse& r, unsigned pageLimit) {
    try {
        require(r.body.view().size() <= kResponseLimit);
        auto requestId = RequestId::parse(header(r, "x-request-id"));
        require(requestId && requestId.value().toString() == header(r, "x-request-id"));
        if (auto it = r.headers.find("content-encoding"); it != r.headers.end()) require(lower(it->second) == "identity");
        ResponseMeta meta{r.status, requestId.value(), {}};
        if (auto it = r.headers.find("etag"); it != r.headers.end()) { require(it->second.size() <= 128); meta.etag = it->second; }
        if (r.status == 204) {
            require(expected(op) == 204 && r.body.view().empty() && !r.headers.count("transfer-encoding"));
            if (auto it = r.headers.find("content-length"); it != r.headers.end()) require(decimal(it->second) == 0);
            if (op == Operation::ChangePassword) require(!meta.etag && !r.headers.count("content-type"));
            return Reply<Payload>{Done{}, std::move(meta)};
        }
        require(jsonType(header(r, "content-type")));
        Document document; document.parse(r.body.view()); const auto& j = document.root;
        require(j.is_object());
        if (r.status != expected(op)) {
            const auto& e = field(j, "error"); const auto& code = string(e, "code");
            require(validError(op, code, r.status) && id<RequestId>(field(e, "request_id")) == meta.requestId);
            require(field(e, "message").is_string() && field(e, "details").is_object());
            auto error = failure(code == "COMMIT_OUTCOME_UNKNOWN" ? ErrorKind::OutcomeUnknown : ErrorKind::Application, code);
            error.remote = code == "COMMIT_OUTCOME_UNKNOWN" ? RemoteOutcome::Unknown : RemoteOutcome::Rejected;
            error.httpStatus = r.status; error.requestId = meta.requestId; return error;
        }
        Payload payload;
        switch (op) {
        case Operation::Register: payload = user(j); break;
        case Operation::Me: case Operation::Rename: {
            auto value = user(j);
            require(meta.etag && *meta.etag == "\"user-" + value.id.toString() + "-" + std::to_string(value.revision.value()) + "\"");
            if (op == Operation::Me) payload = CurrentUser{std::move(value), id<InstanceId>(field(j, "instance_id"))};
            else payload = std::move(value);
            break;
        }
        case Operation::Login: {
            auto token = SecretToken::session(string(j, "access_token")); require(token && string(j, "token_type") == "Bearer");
            payload = Login{user(field(j, "user")), time(field(j, "expires_at")), std::move(token).value()}; break;
        }
        case Operation::CreateInvite: {
            auto token = SecretToken::invite(string(j, "token")); require(static_cast<bool>(token));
            payload = CreatedInvite{invite(field(j, "invite")), std::move(token).value()}; break;
        }
        case Operation::ListInvites: payload = page<Invite>(j, pageLimit, invite); break;
        case Operation::Children: {
            auto value = page<Node>(j, pageLimit, [](const auto& v) { return node(v); });
            for (const auto& item : value.items) require(item.parentId.has_value());
            payload = std::move(value); break;
        }
        case Operation::Spaces: {
            const auto& items = field(j, "items"); require(items.is_array() && items.size() == 2);
            std::vector<Space> value; for (const auto& item : items) value.push_back(space(item));
            require(value[0].id < value[1].id && value[0].kind != value[1].kind); payload = std::move(value); break;
        }
        case Operation::Stat: {
            auto value = node(j); require(meta.etag && *meta.etag == value.etag); payload = std::move(value); break;
        }
        case Operation::Mkdir: {
            require(meta.etag.has_value()); auto value = node(j, meta.etag);
            require(header(r, "location") == "/api/v1/nodes/" + value.id.toString()); payload = std::move(value); break;
        }
        case Operation::GetUser: case Operation::SetUserEnabled: {
            auto value = user(field(j, "user"));
            require(meta.etag && *meta.etag == "\"user-" + value.id.toString() + "-" + std::to_string(value.revision.value()) + "\"");
            payload = AdminUser{std::move(value), id<SpaceId>(field(j, "private_space_id"))}; break;
        }
        case Operation::ListUsers: {
            auto value = page<User>(j, pageLimit, user);
            require(!meta.etag);
            for (std::size_t i = 1; i < value.items.size(); ++i) require(value.items[i-1].id < value.items[i].id);
            payload = std::move(value); break;
        }
        case Operation::GetQuota: case Operation::SetQuotaLimit: {
            QuotaTarget target{id<SpaceId>(field(j, "space_id")), {}};
            if (!field(j, "user_id").is_null()) target.userId = id<UserId>(field(j, "user_id"));
            auto value = quota(field(j, "quota"));
            const auto tag = target.userId ? "\"quota-contribution-" + target.spaceId.toString() + "-" + target.userId->toString() : "\"quota-space-" + target.spaceId.toString();
            require(meta.etag && *meta.etag == tag + "-" + std::to_string(value.revision.value()) + "\"");
            payload = ManagedQuota{target, value}; break;
        }
        case Operation::Logout: case Operation::RevokeInvite: case Operation::ChangePassword: throw InvalidJson{};
        }
        return Reply<Payload>{std::move(payload), std::move(meta)};
    } catch (...) { return failure(writes(op) ? ErrorKind::OutcomeUnknown : ErrorKind::Protocol); }
}
ClientResult<ConditionTag> ConditionTag::parse(std::string_view text) {
    try {
        require(text.size() >= 2 && text.front() == '"' && text.back() == '"' && text.size() <= 128);
        auto rest = text.substr(1, text.size()-2);
        const bool contribution = rest.substr(0,19) == "quota-contribution-";
        if (contribution) rest.remove_prefix(19);
        else if (rest.substr(0,12) == "quota-space-") rest.remove_prefix(12);
        else if (rest.substr(0,5) == "node-") rest.remove_prefix(5);
        else if (rest.substr(0,6) == "trash-") rest.remove_prefix(6);
        else if (rest.substr(0,8) == "storage-") rest.remove_prefix(8);
        else { require(rest.substr(0,5) == "user-"); rest.remove_prefix(5); }
        for (unsigned i=0; i<(contribution?2u:1u); ++i) {
            require(rest.size()>37 && rest[36]=='-');
            auto parsed = UserId::parse(rest.substr(0,36));
            require(parsed && parsed.value().toString()==rest.substr(0,36)); rest.remove_prefix(37);
        }
        decimal(rest,1);
        return ConditionTag(std::string(text));
    } catch (...) { return failure(ErrorKind::InvalidArgument); }
}
ClientResult<std::string> pageQuery(const PageRequest& page) {
    if (page.limit < 1 || page.limit > 200 || (page.cursor && (page.cursor->empty() || page.cursor->size() > 2048))) return failure(ErrorKind::InvalidArgument);
    std::string query = "?limit=" + std::to_string(page.limit);
    if (page.cursor) {
        query += "&cursor=";
        constexpr char hex[] = "0123456789ABCDEF";
        for (unsigned char c : *page.cursor) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') query += static_cast<char>(c);
            else { query += '%'; query += hex[c >> 4]; query += hex[c & 15]; }
        }
    }
    return query;
}
}  // namespace anof::client
