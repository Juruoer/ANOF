#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/internal/FlatJson.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <muduox/net/http/HttpQueryParams.h>
#include <nlohmann/json.hpp>

#include <charconv>
#include <ctime>

namespace anof::http {
namespace {
constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
std::string base64(std::string_view input) {
    std::string output;
    output.reserve((input.size() * 8 + 5) / 6);
    unsigned bits = 0;
    unsigned count = 0;
    for (const unsigned char byte : input) {
        bits = (bits << 8) | byte;
        count += 8;
        while (count >= 6) {
            count -= 6;
            output += alphabet[(bits >> count) & 63];
        }
    }
    if (count) output += alphabet[(bits << (6 - count)) & 63];
    return output;
}
Result<std::string> unbase64(std::string_view input) {
    if (input.empty() || input.size() > kMaxCursorBytes || input.size() % 4 == 1) return ErrorCode::InvalidArgument;
    std::string output;
    output.reserve(input.size() * 3 / 4);
    unsigned bits = 0;
    unsigned count = 0;
    for (const char byte : input) {
        const auto digit = alphabet.find(byte);
        if (digit == std::string_view::npos) return ErrorCode::InvalidArgument;
        bits = (bits << 6) | static_cast<unsigned>(digit);
        count += 6;
        if (count >= 8) {
            count -= 8;
            output += static_cast<char>((bits >> count) & 255);
        }
    }
    if ((count && (bits & ((1U << count) - 1)) != 0) || output.size() > kMaxDecodedCursorBytes) return ErrorCode::InvalidArgument;
    return output;
}
template <typename IdType>
Result<IdType> canonicalId(const std::string& text) {
    auto id = IdType::parse(text);
    if (!id || id.value().toString() != text) return ErrorCode::InvalidArgument;
    return id;
}
struct Query {
    PageSize size = PageSize::from(100).value();
    std::optional<std::string> cursor;
};
Parsed<Query> parsePageQuery(std::string_view query) {
    auto parsed = muduox::net::parseHttpQuery(query, muduox::net::HttpQueryMode::Percent, {8192, 2, 4096});
    auto* parameters = std::get_if<muduox::net::HttpQueryParams>(&parsed);
    if (!parameters) return ProtocolError::InvalidArgument;
    Query result;
    bool hasLimit = false;
    for (const auto& parameter : *parameters) {
        const auto& value = parameter.value;
        if (!parameter.hasEquals || value.empty()) return ProtocolError::InvalidArgument;
        if (parameter.name == "limit") {
            if (hasLimit || value.front() < '1' || value.front() > '9') return ProtocolError::InvalidArgument;
            hasLimit = true;
            std::int64_t number = 0;
            const auto converted = std::from_chars(value.data(), value.data() + value.size(), number);
            if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) return ProtocolError::InvalidArgument;
            auto size = PageSize::from(number);
            if (!size) return ProtocolError::InvalidArgument;
            result.size = size.value();
        } else if (parameter.name == "cursor") {
            if (result.cursor || value.size() > kMaxCursorBytes) return ProtocolError::InvalidArgument;
            result.cursor = value;
        } else return ProtocolError::InvalidArgument;
    }
    return result;
}
Result<std::string> encoded(const nlohmann::json& json) {
    const auto text = json.dump();
    if (text.size() > kMaxDecodedCursorBytes) return ErrorCode::InternalError;
    return base64(text);
}
}  // namespace

Result<UtcTime> parseUtcTimestamp(std::string_view text) {
    if (text.size() != 27 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != '.' || text[26] != 'Z') return ErrorCode::InvalidArgument;
    auto number = [&](std::size_t offset, std::size_t count) {
        int result = 0;
        for (std::size_t i = offset; i < offset + count; ++i) {
            if (text[i] < '0' || text[i] > '9') return -1;
            result = result * 10 + (text[i] - '0');
        }
        return result;
    };
    const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
    const int hour = number(11, 2), minute = number(14, 2), second = number(17, 2), micros = number(20, 6);
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 || micros < 0) return ErrorCode::InvalidArgument;
    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    const auto seconds = ::timegm(&utc);
    // 年份已限定为四位；归一化后严格回比以拒绝 2 月 30 日等日期。
    const auto value = UtcTime(std::chrono::microseconds(static_cast<std::int64_t>(seconds) * 1000000 + micros));
    const auto canonical = formatUtcTimestamp(value);
    if (!canonical || canonical.value() != text) return ErrorCode::InvalidArgument;
    return value;
}
Parsed<ChildrenCursor> decodeChildrenCursor(std::string_view cursor) {
    auto decoded = unbase64(cursor);
    if (!decoded) return ProtocolError::InvalidArgument;
    internal::FlatJson json{{"v", internal::FieldType::Integer}, {"kind"}, {"user_id"}, {"space_id"}, {"parent_id"}, {"after_name"}, {"after_id"}};
    if (json.parse(decoded.value(), kMaxDecodedCursorBytes) || json[0].integer != 1 || json[1].text != "children") return ProtocolError::InvalidArgument;
    auto user = canonicalId<UserId>(json[2].text);
    auto space = canonicalId<SpaceId>(json[3].text);
    auto parent = canonicalId<NodeId>(json[4].text);
    auto after = canonicalId<NodeId>(json[6].text);
    if (!user || !space || !parent || !after || !validateDirectoryName(json[5].text)) return ProtocolError::InvalidArgument;
    return ChildrenCursor{user.value(), space.value(), parent.value(), {json[5].text, after.value()}};
}
Parsed<InviteCursor> decodeInviteCursor(std::string_view cursor) {
    auto decoded = unbase64(cursor);
    if (!decoded) return ProtocolError::InvalidArgument;
    internal::FlatJson json{{"v", internal::FieldType::Integer}, {"kind"}, {"user_id"}, {"after_created_at"}, {"after_id"}};
    if (json.parse(decoded.value(), kMaxDecodedCursorBytes) || json[0].integer != 1 || json[1].text != "invites") return ProtocolError::InvalidArgument;
    auto user = canonicalId<UserId>(json[2].text);
    auto time = parseUtcTimestamp(json[3].text);
    auto after = canonicalId<InviteId>(json[4].text);
    if (!user || !time || !after) return ProtocolError::InvalidArgument;
    return InviteCursor{user.value(), {time.value(), after.value()}};
}
Result<std::string> encodeCursor(const ChildrenCursor& cursor) {
    if (!validateDirectoryName(cursor.position.name)) return ErrorCode::InternalError;
    return encoded({{"v", 1}, {"kind", "children"}, {"user_id", cursor.userId.toString()}, {"space_id", cursor.spaceId.toString()}, {"parent_id", cursor.parent.toString()}, {"after_name", cursor.position.name}, {"after_id", cursor.position.id.toString()}});
}
Result<std::string> encodeCursor(const InviteCursor& cursor) {
    auto time = formatUtcTimestamp(cursor.position.createdAt);
    if (!time) return time.error();
    return encoded({{"v", 1}, {"kind", "invites"}, {"user_id", cursor.userId.toString()}, {"after_created_at", time.value()}, {"after_id", cursor.position.id.toString()}});
}
Parsed<ListChildrenCommand> parseListChildren(std::string_view parent, std::string_view query) {
    auto id = NodeId::parse(parent);
    auto parsed = parsePageQuery(query);
    if (!id || std::holds_alternative<ProtocolError>(parsed)) return ProtocolError::InvalidArgument;
    auto& params = std::get<Query>(parsed);
    std::optional<ChildrenCursor> cursor;
    if (params.cursor) {
        auto value = decodeChildrenCursor(*params.cursor);
        if (std::holds_alternative<ProtocolError>(value)) return ProtocolError::InvalidArgument;
        cursor = std::get<ChildrenCursor>(std::move(value));
    }
    // 不比较 cursor.parent；应用先授权并验证目标可见性，再检查绑定。
    return ListChildrenCommand{id.value(), params.size, std::move(cursor)};
}
Parsed<ListInvitesCommand> parseListInvites(std::string_view query) {
    auto parsed = parsePageQuery(query);
    if (const auto* error = std::get_if<ProtocolError>(&parsed)) return *error;
    auto& params = std::get<Query>(parsed);
    std::optional<InviteCursor> cursor;
    if (params.cursor) {
        auto value = decodeInviteCursor(*params.cursor);
        if (std::holds_alternative<ProtocolError>(value)) return ProtocolError::InvalidArgument;
        cursor = std::get<InviteCursor>(std::move(value));
    }
    return ListInvitesCommand{params.size, std::move(cursor)};
}
Parsed<UserCursor> decodeUserCursor(std::string_view cursor) {
    auto decoded = unbase64(cursor);
    if (!decoded) return ProtocolError::InvalidArgument;
    internal::FlatJson json{{"v", internal::FieldType::Integer}, {"kind"}, {"user_id"}, {"after_id"}};
    if (json.parse(decoded.value(), kMaxDecodedCursorBytes) || json[0].integer != 1 || json[1].text != "users") return ProtocolError::InvalidArgument;
    auto user = canonicalId<UserId>(json[2].text);
    auto after = canonicalId<UserId>(json[3].text);
    if (!user || !after) return ProtocolError::InvalidArgument;
    return UserCursor{user.value(), {after.value()}};
}
Result<std::string> encodeCursor(const UserCursor& cursor) {
    return encoded({{"v", 1}, {"kind", "users"}, {"user_id", cursor.userId.toString()}, {"after_id", cursor.position.id.toString()}});
}
Parsed<ListUsersCommand> parseListUsers(std::string_view query) {
    auto parsed = parsePageQuery(query);
    if (const auto* error = std::get_if<ProtocolError>(&parsed)) return *error;
    auto& params = std::get<Query>(parsed);
    std::optional<UserCursor> cursor;
    if (params.cursor) {
        auto value = decodeUserCursor(*params.cursor);
        if (const auto* error = std::get_if<ProtocolError>(&value)) return *error;
        cursor = std::get<UserCursor>(std::move(value));
    }
    return ListUsersCommand{params.size, std::move(cursor)};
}
Parsed<UploadCursor> decodeUploadCursor(std::string_view cursor) {
    auto decoded = unbase64(cursor);
    if (!decoded) return ProtocolError::InvalidArgument;
    internal::FlatJson json{{"v", internal::FieldType::Integer}, {"kind"}, {"user_id"}, {"after_created_at"}, {"after_id"}};
    if (json.parse(decoded.value(), kMaxDecodedCursorBytes) || json[0].integer != 1 || json[1].text != "uploads") return ProtocolError::InvalidArgument;
    auto user = canonicalId<UserId>(json[2].text);
    auto time = parseUtcTimestamp(json[3].text);
    auto after = canonicalId<UploadId>(json[4].text);
    if (!user || !time || !after) return ProtocolError::InvalidArgument;
    return UploadCursor{user.value(), {time.value(), after.value()}};
}
Result<std::string> encodeCursor(const UploadCursor& cursor) {
    auto time = formatUtcTimestamp(cursor.position.createdAt);
    if (!time) return time.error();
    return encoded({{"v", 1}, {"kind", "uploads"}, {"user_id", cursor.userId.toString()}, {"after_created_at", time.value()}, {"after_id", cursor.position.id.toString()}});
}
Parsed<ListUploadsCommand> parseListUploads(std::string_view query) {
    auto parsed = muduox::net::parseHttpQuery(query, muduox::net::HttpQueryMode::Percent, {8192, 3, 4096});
    auto* params = std::get_if<muduox::net::HttpQueryParams>(&parsed);
    if (!params) return ProtocolError::InvalidArgument;
    ListUploadsCommand result{PageSize::from(100).value(), {}, {}};
    bool limit = false;
    for (const auto& p : *params) {
        if (!p.hasEquals || p.value.empty()) return ProtocolError::InvalidArgument;
        if (p.name == "limit") {
            auto number = parseTransferDecimal(p.value);
            if (limit || !number) return ProtocolError::InvalidArgument;
            auto size = PageSize::from(number.value().value());
            if (!size) return ProtocolError::InvalidArgument;
            result.size = size.value(); limit = true;
        } else if (p.name == "cursor") {
            if (result.after || result.clientUploadId) return ProtocolError::InvalidArgument;
            auto value = decodeUploadCursor(p.value);
            if (auto* error = std::get_if<ProtocolError>(&value)) return *error;
            result.after = std::get<UploadCursor>(value);
        } else if (p.name == "client_upload_id") {
            if (result.after || result.clientUploadId) return ProtocolError::InvalidArgument;
            auto id = canonicalId<OperationId>(p.value);
            if (!id) return ProtocolError::InvalidArgument;
            result.clientUploadId = id.value();
        } else return ProtocolError::InvalidArgument;
    }
    return result;
}

Parsed<SearchCursor> decodeSearchCursor(std::string_view cursor) {
    auto decoded = unbase64(cursor);
    if (!decoded) return ProtocolError::InvalidArgument;
    internal::FlatJson json{{"v", internal::FieldType::Integer}, {"kind"}, {"sort"}, {"user_id"}, {"space_id"}, {"q"}, {"after_name"}, {"after_id"}};
    if (json.parse(decoded.value(), kMaxDecodedCursorBytes) || json[0].integer != 1 || json[1].text != "search" || json[2].text != "name-id-v1") return ProtocolError::InvalidArgument;
    auto user = canonicalId<UserId>(json[3].text);
    auto space = canonicalId<SpaceId>(json[4].text);
    auto text = SearchText::parse(json[5].text);
    auto id = canonicalId<NodeId>(json[7].text);
    if (!user || !space || !text || !id || !validateDirectoryName(json[6].text)) return ProtocolError::InvalidArgument;
    return SearchCursor{user.value(), space.value(), text.value(), {json[6].text, id.value()}};
}
Parsed<TrashCursor> decodeTrashCursor(std::string_view cursor) {
    auto decoded = unbase64(cursor);
    if (!decoded) return ProtocolError::InvalidArgument;
    internal::FlatJson json{{"v", internal::FieldType::Integer}, {"kind"}, {"sort"}, {"user_id"}, {"space_id"}, {"after_deleted_at"}, {"after_id"}, {"root_node_id", internal::FieldType::Text, false}};
    if (json.parse(decoded.value(), kMaxDecodedCursorBytes) || json[0].integer != 1 || json[1].text != "trash" || json[2].text != "deleted-at-id-v1") return ProtocolError::InvalidArgument;
    auto user = canonicalId<UserId>(json[3].text);
    auto space = canonicalId<SpaceId>(json[4].text);
    auto time = parseUtcTimestamp(json[5].text);
    auto id = canonicalId<TrashId>(json[6].text);
    if (!user || !space || !time || !id) return ProtocolError::InvalidArgument;
    std::optional<NodeId> root;
    if (json[7].seen) {
        auto value = canonicalId<NodeId>(json[7].text);
        if (!value) return ProtocolError::InvalidArgument;
        root = value.value();
    }
    return TrashCursor{user.value(), space.value(), root, {time.value(), id.value()}};
}
Result<std::string> encodeCursor(const SearchCursor& cursor) {
    if (!validateDirectoryName(cursor.position.name)) return ErrorCode::InternalError;
    return encoded({{"v", 1}, {"kind", "search"}, {"sort", "name-id-v1"}, {"user_id", cursor.user.toString()}, {"space_id", cursor.space.toString()}, {"q", cursor.text.value()}, {"after_name", cursor.position.name}, {"after_id", cursor.position.id.toString()}});
}
Result<std::string> encodeCursor(const TrashCursor& cursor) {
    auto time = formatUtcTimestamp(cursor.position.deletedAt);
    if (!time) return time.error();
    nlohmann::json json{{"v", 1}, {"kind", "trash"}, {"sort", "deleted-at-id-v1"}, {"user_id", cursor.user.toString()}, {"space_id", cursor.space.toString()}, {"after_deleted_at", time.value()}, {"after_id", cursor.position.id.toString()}};
    if (cursor.root) json["root_node_id"] = cursor.root->toString();
    return encoded(json);
}
namespace {
struct DriveQuery {
    std::optional<SpaceId> space;
    std::optional<NodeId> root;
    std::optional<SearchText> text;
    PageSize limit = PageSize::from(100).value();
    std::optional<std::string> cursor;
};
Parsed<DriveQuery> driveQuery(std::string_view input, bool search) {
    auto parsed = muduox::net::parseHttpQuery(input, muduox::net::HttpQueryMode::Percent, {8192, 5, 4096});
    auto* params = std::get_if<muduox::net::HttpQueryParams>(&parsed);
    if (!params) return ProtocolError::InvalidArgument;
    DriveQuery result;
    bool limit = false;
    for (const auto& p : *params) {
        if (!p.hasEquals || p.value.empty()) return ProtocolError::InvalidArgument;
        if (p.name == "space_id") {
            auto id = SpaceId::parse(p.value);
            if (result.space || !id) return ProtocolError::InvalidArgument;
            result.space = id.value();
        } else if (p.name == "q" && search) {
            auto text = SearchText::parse(p.value);
            if (result.text || !text) return ProtocolError::InvalidArgument;
            result.text = text.value();
        } else if (p.name == "root_node_id" && !search) {
            auto id = NodeId::parse(p.value);
            if (result.root || !id) return ProtocolError::InvalidArgument;
            result.root = id.value();
        } else if (p.name == "limit") {
            auto number = parseTransferDecimal(p.value);
            if (limit || !number) return ProtocolError::InvalidArgument;
            auto size = PageSize::from(number.value().value());
            if (!size) return ProtocolError::InvalidArgument;
            result.limit = size.value(); limit = true;
        } else if (p.name == "cursor") {
            if (result.cursor || p.value.size() > kMaxCursorBytes) return ProtocolError::InvalidArgument;
            result.cursor = p.value;
        } else return ProtocolError::InvalidArgument;
    }
    if (!result.space || (search && !result.text)) return ProtocolError::InvalidArgument;
    return result;
}
}
Parsed<SearchNodesCommand> parseSearchNodes(std::string_view query) {
    auto parsed = driveQuery(query, true);
    if (auto* error = std::get_if<ProtocolError>(&parsed)) return *error;
    auto value = std::get<DriveQuery>(std::move(parsed));
    std::optional<SearchCursor> cursor;
    if (value.cursor) {
        auto decoded = decodeSearchCursor(*value.cursor);
        if (auto* error = std::get_if<ProtocolError>(&decoded)) return *error;
        cursor = std::get<SearchCursor>(std::move(decoded));
    }
    return SearchNodesCommand{*value.space, *value.text, value.limit, std::move(cursor)};
}
Parsed<ListTrashCommand> parseListTrash(std::string_view query) {
    auto parsed = driveQuery(query, false);
    if (auto* error = std::get_if<ProtocolError>(&parsed)) return *error;
    auto value = std::get<DriveQuery>(std::move(parsed));
    std::optional<TrashCursor> cursor;
    if (value.cursor) {
        auto decoded = decodeTrashCursor(*value.cursor);
        if (auto* error = std::get_if<ProtocolError>(&decoded)) return *error;
        cursor = std::get<TrashCursor>(std::move(decoded));
    }
    return ListTrashCommand{*value.space, value.root, value.limit, std::move(cursor)};
}
}  // namespace anof::http
