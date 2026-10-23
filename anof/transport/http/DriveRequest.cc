#include <anof/transport/http/DriveRequest.h>
#include <anof/transport/http/internal/FlatJson.h>
#include <anof/domain/drive/DirectoryPolicy.h>
namespace anof::http {
namespace {
struct Target { std::optional<NodeId> parent; std::optional<std::string> name; };
Parsed<Target> target(std::string_view body, bool required) {
    internal::FlatJson json{{"parent_id", internal::FieldType::Text, false}, {"name", internal::FieldType::Text, false}};
    if (auto error = json.parse(body)) return *error;
    if (required && !json[0].seen && !json[1].seen) return ProtocolError::InvalidArgument;
    Target value;
    if (json[0].seen) {
        auto id = NodeId::parse(json[0].text);
        if (!id) return ProtocolError::InvalidArgument;
        value.parent = id.value();
    }
    if (json[1].seen) {
        if (!validateDirectoryName(json[1].text)) return ProtocolError::InvalidName;
        value.name = json[1].text;
    }
    return value;
}
}
Parsed<DriveCondition> parseDriveCondition(const muduox::net::HttpHeaders& headers) {
    using Status = muduox::net::HttpHeaders::SingleStatus;
    auto field = headers.getSingle("If-Match");
    if (field.status == Status::Missing) return ProtocolError::PreconditionRequired;
    if (field.status != Status::Unique) return ProtocolError::InvalidArgument;
    auto tag = field.value;
    while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) tag.remove_prefix(1);
    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) tag.remove_suffix(1);
    if (tag.size() < 2 || tag.front() != '"' || tag.back() != '"') return ProtocolError::InvalidArgument;
    tag.remove_prefix(1); tag.remove_suffix(1);
    const bool node = tag.substr(0, 5) == "node-";
    if (!node && tag.substr(0, 6) != "trash-") return ProtocolError::InvalidArgument;
    tag.remove_prefix(node ? 5 : 6);
    if (tag.size() < 38 || tag[36] != '-') return ProtocolError::InvalidArgument;
    auto revision = parseTransferDecimal(tag.substr(37));
    if (!revision || revision.value().value() == 0) return ProtocolError::InvalidArgument;
    const auto version = Revision::from(revision.value().value()).value();
    const auto raw = tag.substr(0, 36);
    // 按标签自身种类构造，不比较路由、ID或版本；权限与业务条件必须在同一任务中判断。
    if (node) {
        auto id = NodeId::parse(raw);
        if (!id || id.value().toString() != raw) return ProtocolError::InvalidArgument;
        return DriveCondition{NodeCondition{id.value(), version}};
    }
    auto id = TrashId::parse(raw);
    if (!id || id.value().toString() != raw) return ProtocolError::InvalidArgument;
    return DriveCondition{TrashCondition{id.value(), version}};
}
Parsed<UpdateNodeCommand> parseUpdateNode(std::string_view body, NodeId id, DriveCondition condition) {
    auto value = target(body, true);
    if (auto* error = std::get_if<ProtocolError>(&value)) return *error;
    auto result = std::get<Target>(std::move(value));
    return UpdateNodeCommand{id, std::move(condition), result.parent, std::move(result.name)};
}
Parsed<CopyFileCommand> parseCopyFile(std::string_view body, NodeId id, DriveCondition condition) {
    internal::FlatJson json{{"space_id"}, {"parent_id"}, {"name"}};
    if (auto error = json.parse(body)) return *error;
    auto space = SpaceId::parse(json[0].text);
    auto parent = NodeId::parse(json[1].text);
    if (!space || !parent) return ProtocolError::InvalidArgument;
    if (!validateDirectoryName(json[2].text)) return ProtocolError::InvalidName;
    return CopyFileCommand{id, std::move(condition), space.value(), parent.value(), json[2].text};
}
Parsed<RestoreTrashCommand> parseRestoreTrash(std::string_view body, TrashId id, DriveCondition condition) {
    auto value = target(body, false);
    if (auto* error = std::get_if<ProtocolError>(&value)) return *error;
    auto result = std::get<Target>(std::move(value));
    return RestoreTrashCommand{id, std::move(condition), result.parent, std::move(result.name)};
}
}  // namespace anof::http
