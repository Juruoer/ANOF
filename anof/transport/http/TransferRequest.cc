#include <anof/transport/http/TransferRequest.h>
#include <anof/transport/http/internal/FlatJson.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <charconv>
namespace anof::http {
namespace {
template<typename T> Result<T> canonical(std::string_view value) {
    auto id = T::parse(value);
    if (!id || id.value().toString() != value) return ErrorCode::InvalidArgument;
    return id;
}
}
Parsed<UploadId> parseUploadId(std::string_view text) {
    auto id = canonical<UploadId>(text);
    if (!id) return ProtocolError::InvalidArgument;
    return id.value();
}
Parsed<CreateUploadCommand> parseCreateUpload(std::string_view body) {
    internal::FlatJson json{{"client_upload_id"}, {"space_id"}, {"parent_id"}, {"name"}, {"length"}, {"sha256", internal::FieldType::Text, false}};
    if (auto error = json.parse(body)) return *error;
    auto operation = canonical<OperationId>(json[0].text);
    auto space = canonical<SpaceId>(json[1].text);
    auto parent = canonical<NodeId>(json[2].text);
    auto count = parseTransferDecimal(json[4].text);
    if (!operation || !space || !parent || !count) return ProtocolError::InvalidArgument;
    auto length = FileLength::from(count.value().value());
    if (!length) return ProtocolError::InvalidArgument;
    if (!validateDirectoryName(json[3].text)) return ProtocolError::InvalidName;
    std::optional<Sha256> hash;
    if (json[5].seen) {
        auto parsed = Sha256::parse(json[5].text);
        if (!parsed) return ProtocolError::InvalidArgument;
        hash = parsed.value();
    }
    return CreateUploadCommand{operation.value(), {space.value(), {parent.value(), json[3].text}, length.value(), hash}};
}
Parsed<CompleteUploadCommand> parseCompleteUpload(std::string_view body, UploadId id) {
    internal::FlatJson json{{"parent_id", internal::FieldType::Text, false}, {"name", internal::FieldType::Text, false}};
    if (auto error = json.parse(body)) return *error;
    if (json[0].seen != json[1].seen) return ProtocolError::InvalidArgument;
    if (!json[0].seen) return CompleteUploadCommand{id, std::nullopt};
    auto parent = canonical<NodeId>(json[0].text);
    if (!parent) return ProtocolError::InvalidArgument;
    if (!validateDirectoryName(json[1].text)) return ProtocolError::InvalidName;
    return CompleteUploadCommand{id, UploadTarget{parent.value(), json[1].text}};
}
Parsed<StorageCondition> parseStorageCondition(const muduox::net::HttpHeaders& headers) {
    using Status = muduox::net::HttpHeaders::SingleStatus;
    auto header = headers.getSingle("if-match");
    if (header.status == Status::Missing) return ProtocolError::PreconditionRequired;
    if (header.status != Status::Unique) return ProtocolError::InvalidArgument;
    auto tag = header.value;
    while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) tag.remove_prefix(1);
    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) tag.remove_suffix(1);
    if (tag.size() < 48 || tag.substr(0, 9) != "\"storage-" || tag.back() != '"' || tag[45] != '-') return ProtocolError::InvalidArgument;
    auto id = canonical<InstanceId>(tag.substr(9, 36));
    auto revision = parseTransferDecimal(tag.substr(46, tag.size() - 47));
    if (!id || !revision || revision.value().value() == 0) return ProtocolError::InvalidArgument;
    return StorageCondition{id.value(), Revision::from(revision.value().value()).value()};
}
Parsed<SetStorageBudgetCommand> parseStorageUpdate(std::string_view body, StorageCondition condition) {
    internal::FlatJson json{{"content_budget_bytes"}};
    if (auto error = json.parse(body)) return *error;
    auto budget = parseTransferDecimal(json[0].text);
    if (!budget) return ProtocolError::InvalidArgument;
    return SetStorageBudgetCommand{condition, budget.value()};
}
Parsed<PreparePatchCommand> parsePatchHeaders(const muduox::net::HttpHeaders& headers, UploadId id) {
    using Status = muduox::net::HttpHeaders::SingleStatus;
    auto cl = headers.getSingle("content-length"), te = headers.getSingle("transfer-encoding");
    if (te.status != Status::Missing) return cl.status != Status::Missing ? ProtocolError::InvalidArgument : ProtocolError::UnsupportedMediaType;
    if (cl.status == Status::Missing) return ProtocolError::LengthRequired;
    if (cl.status != Status::Unique || cl.value.empty() || cl.value.find_first_not_of("0123456789") != std::string_view::npos) return ProtocolError::InvalidArgument;
    std::uint64_t length = 0;
    const auto converted = std::from_chars(cl.value.data(), cl.value.data() + cl.value.size(), length);
    if (converted.ec != std::errc{} || length > static_cast<std::uint64_t>(kMaxPatchBytes)) return ProtocolError::RequestTooLarge;
    if (!length) return ProtocolError::InvalidArgument;
    auto offset = headers.getSingle("upload-offset");
    if (offset.status != Status::Unique) return ProtocolError::InvalidArgument;
    auto number = parseTransferDecimal(offset.value);
    if (!number) return ProtocolError::InvalidArgument;
    auto media = headers.getSingle("content-type"), encoding = headers.getSingle("content-encoding");
    if (media.status == Status::Multiple || encoding.status == Status::Multiple) return ProtocolError::InvalidArgument;
    if (media.status != Status::Unique || media.value != "application/offset+octet-stream" ||
        (encoding.status != Status::Missing && encoding.value != "identity")) return ProtocolError::UnsupportedMediaType;
    if (headers.getSingle("trailer").status != Status::Missing) return ProtocolError::InvalidArgument;
    return PreparePatchCommand{id, UploadOffset::from(number.value().value()).value(), ByteCount::from(static_cast<std::int64_t>(length)).value()};
}
}  // namespace anof::http
