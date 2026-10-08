#define BOOST_TEST_MODULE HttpProtocol
#include <boost/test/unit_test.hpp>
#include <anof/transport/http/BearerCredentials.h>
#include <anof/transport/http/CreateDirectoryJson.h>
#include <muduox/net/Buffer.h>
#include <nlohmann/json.hpp>

#include <limits>

using namespace anof;
using namespace anof::http;
using muduox::net::HttpHeaders;
namespace {
const std::string space = "AAAAAAAA-0000-4000-8000-000000000001";
const std::string parent = "20000000-0000-4000-8000-000000000001";
RequestId requestId() { return RequestId::parse("50000000-0000-4000-8000-000000000001").value(); }
std::string request(std::string name = "家庭照片") {
    return nlohmann::json{{"space_id", space}, {"parent_id", parent}, {"name", name}}.dump();
}
void expectError(std::string_view body, ProtocolError error) {
    const auto result = parseCreateDirectoryJson(body);
    BOOST_REQUIRE(std::holds_alternative<ProtocolError>(result));
    BOOST_CHECK(std::get<ProtocolError>(result) == error);
}
class Codec final : public SessionTokenCodec {
public:
    mutable int calls = 0;
    Result<SessionDigest> digestBearer(std::string_view token) const override {
        ++calls;
        if (token == "opaque") return SessionDigest(SessionDigest::Bytes{});
        if (token == "internal") return ErrorCode::InternalError;
        return ErrorCode::Unauthenticated;
    }
};
CreateDirectoryResult created() {
    return {{NodeId::parse("30000000-0000-4000-8000-000000000001").value(), SpaceId::parse(space).value(),
             NodeId::parse(parent).value(), "家庭照片", UserId::parse("40000000-0000-4000-8000-000000000001").value(),
             Revision::from(1).value(), UtcTime{}, UtcTime{}}, Revision::from(2).value()};
}
}  // namespace

BOOST_AUTO_TEST_CASE(bearerExtractionAndCodecBoundary) {
    Codec codec;
    HttpHeaders headers;
    headers.append("Authorization", "bEaReR   opaque");
    BOOST_CHECK(std::holds_alternative<SessionDigest>(readBearerCredentials(headers, codec)));
    BOOST_CHECK_EQUAL(codec.calls, 1);
    headers.set("Authorization", "Bearer internal");
    BOOST_CHECK(std::get<ProtocolError>(readBearerCredentials(headers, codec)) == ProtocolError::InternalError);
    headers.set("Authorization", "Bearer noncanonical");
    BOOST_CHECK(std::get<ProtocolError>(readBearerCredentials(headers, codec)) == ProtocolError::Unauthenticated);
    BOOST_CHECK_EQUAL(codec.calls, 3);
}
BOOST_AUTO_TEST_CASE(ambiguousAndMalformedCredentials) {
    Codec codec;
    for (const auto& text : {"", "Basic opaque", "Bearer", "Bearer\topaque", "Bearer opaque extra", "Bearer opaque,extra"}) {
        HttpHeaders headers;
        headers.append("Authorization", text);
        BOOST_CHECK(std::get<ProtocolError>(readBearerCredentials(headers, codec)) == ProtocolError::Unauthenticated);
    }
    HttpHeaders headers;
    BOOST_CHECK(std::get<ProtocolError>(readBearerCredentials(headers, codec)) == ProtocolError::Unauthenticated);
    headers.append("Authorization", "Bearer opaque");
    headers.append("authorization", "Bearer opaque");
    BOOST_CHECK(std::get<ProtocolError>(readBearerCredentials(headers, codec)) == ProtocolError::InvalidArgument);
    headers.set("Authorization", "Bearer opaque");
    headers.append("Cookie", "");
    BOOST_CHECK(std::get<ProtocolError>(readBearerCredentials(headers, codec)) == ProtocolError::InvalidArgument);
    BOOST_CHECK_EQUAL(codec.calls, 0);
}
BOOST_AUTO_TEST_CASE(contentHeaders) {
    for (const auto& type : {"application/json", "Application/JSON; Charset=\"UTF-8\"", " application/json ; charset = utf-8 "}) {
        HttpHeaders headers;
        headers.append("Content-Type", type);
        BOOST_CHECK(!validateJsonContentHeaders(headers));
        headers.append("Content-Encoding", "IDENTITY");
        BOOST_CHECK(!validateJsonContentHeaders(headers));
        headers.append("content-encoding", "identity");
        BOOST_CHECK(*validateJsonContentHeaders(headers) == ProtocolError::InvalidArgument);
    }
    for (const auto& type : {"", "text/json", "application/json;", "application/json; charset=ascii", "application/json; charset=utf-8; charset=utf-8", "application/json; other=utf-8", "application/json; charset=\"utf-8", "application/json, application/json"}) {
        HttpHeaders headers;
        headers.append("Content-Type", type);
        BOOST_CHECK(*validateJsonContentHeaders(headers) == ProtocolError::UnsupportedMediaType);
    }
    HttpHeaders headers;
    BOOST_CHECK(*validateJsonContentHeaders(headers) == ProtocolError::UnsupportedMediaType);
    headers.set("Content-Type", "application/json");
    headers.append("CONTENT-TYPE", "application/json");
    BOOST_CHECK(*validateJsonContentHeaders(headers) == ProtocolError::InvalidArgument);
    headers.set("Content-Type", "application/json");
    for (const auto& encoding : {"gzip", "", "identity, identity"}) {
        headers.set("Content-Encoding", encoding);
        BOOST_CHECK(*validateJsonContentHeaders(headers) == ProtocolError::UnsupportedMediaType);
    }
}
BOOST_AUTO_TEST_CASE(validCommandOwnsAndNormalizesOnlyIds) {
    auto body = request(" A ");
    auto result = parseCreateDirectoryJson(body);
    body.assign(body.size(), 'x');
    BOOST_REQUIRE(std::holds_alternative<CreateDirectoryCommand>(result));
    const auto& command = std::get<CreateDirectoryCommand>(result);
    BOOST_CHECK_EQUAL(command.spaceId.toString(), "aaaaaaaa-0000-4000-8000-000000000001");
    BOOST_CHECK_EQUAL(command.parentId.toString(), parent);
    BOOST_CHECK_EQUAL(command.name, " A ");
}
BOOST_AUTO_TEST_CASE(jsonStructureAndDecodedDuplicates) {
    for (const auto& body : {"", "[]", "null", "42", "\"name\"", "{}", "{", "{}{}", "/*x*/{}"}) expectError(body, ProtocolError::InvalidArgument);
    auto body = request();
    for (const auto& extra : {",\"name\":\"x\"}", ",\"na\\u006de\":\"x\"}", ",\"unknown\":\"x\"}"}) {
        expectError(body.substr(0, body.size() - 1) + extra, ProtocolError::InvalidArgument);
    }
    for (const auto& value : {"null", "true", "1", "1.5", "[]", "{}"}) {
        expectError("{\"space_id\":\"" + space + "\",\"parent_id\":\"" + parent + "\",\"name\":" + value + "}", ProtocolError::InvalidArgument);
    }
    expectError(body + " trailing", ProtocolError::InvalidArgument);
    auto json = nlohmann::json::parse(body);
    json["space_id"] = "00000000-0000-0000-0000-000000000000";
    expectError(json.dump(), ProtocolError::InvalidArgument);
    json["space_id"] = space;
    json["parent_id"] = "bad";
    expectError(json.dump(), ProtocolError::InvalidArgument);
}
BOOST_AUTO_TEST_CASE(unicodeAndByteLimits) {
    std::string name;
    for (int i = 0; i < 85; ++i) name += "中";
    BOOST_CHECK(std::holds_alternative<CreateDirectoryCommand>(parseCreateDirectoryJson(request(name))));
    expectError(request(name + "中"), ProtocolError::InvalidName);
    BOOST_CHECK(std::holds_alternative<CreateDirectoryCommand>(parseCreateDirectoryJson(request(std::string(255, 'x')))));
    expectError(request(std::string(256, 'x')), ProtocolError::InvalidName);
    for (const auto& invalid : {std::string{}, std::string("."), std::string(".."), std::string("a/b"), std::string("a\\b"), std::string("\0", 1), std::string("\xc2\x85", 2)}) expectError(request(invalid), ProtocolError::InvalidName);
    const auto prefix = "{\"space_id\":\"" + space + "\",\"parent_id\":\"" + parent + "\",\"name\":\"";
    expectError(prefix + "\\ud800\"}", ProtocolError::InvalidArgument);
    expectError(prefix + std::string("\xc0\xaf", 2) + "\"}", ProtocolError::InvalidArgument);
    BOOST_CHECK(std::holds_alternative<CreateDirectoryCommand>(parseCreateDirectoryJson(prefix + "\\ud83d\\ude00\"}")));
}
BOOST_AUTO_TEST_CASE(bodyAndNestingBounds) {
    auto body = request();
    body.resize(kMaxJsonBytes, ' ');
    BOOST_CHECK(std::holds_alternative<CreateDirectoryCommand>(parseCreateDirectoryJson(body)));
    body += ' ';
    expectError(body, ProtocolError::RequestTooLarge);
    for (std::size_t depth : {2U, 32U, 33U, 10000U}) {
        expectError(std::string(depth, '[') + "0" + std::string(depth, ']'), ProtocolError::InvalidArgument);
        expectError("{\"name\":" + std::string(depth, '[') + "0" + std::string(depth, ']') + "}", ProtocolError::InvalidArgument);
    }
}
BOOST_AUTO_TEST_CASE(timestampBoundaries) {
    const auto check = [](std::int64_t micros, const char* expected) {
        const auto result = formatUtcTimestamp(UtcTime(std::chrono::microseconds(micros)));
        BOOST_REQUIRE(result);
        BOOST_CHECK_EQUAL(result.value(), expected);
    };
    check(0, "1970-01-01T00:00:00.000000Z");
    check(-1, "1969-12-31T23:59:59.999999Z");
    check(951782400123456LL, "2000-02-29T00:00:00.123456Z");
    check(-62135596800000000LL, "0001-01-01T00:00:00.000000Z");
    check(253402300799999999LL, "9999-12-31T23:59:59.999999Z");
    for (auto micros : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max(), std::int64_t{-62135596800000001LL}, std::int64_t{253402300800000000LL}}) BOOST_CHECK(!formatUtcTimestamp(UtcTime(std::chrono::microseconds(micros))));
}
BOOST_AUTO_TEST_CASE(successRepresentationAndHeaders) {
    auto result = createdDirectoryResponse(created(), requestId());
    BOOST_REQUIRE(result);
    const auto& response = result.value();
    const auto json = nlohmann::json::parse(response.body);
    const nlohmann::json expected = {
        {"id", "30000000-0000-4000-8000-000000000001"},
        {"space_id", "aaaaaaaa-0000-4000-8000-000000000001"},
        {"parent_id", parent}, {"name", "家庭照片"}, {"kind", "directory"},
        {"created_by", "40000000-0000-4000-8000-000000000001"},
        {"created_at", "1970-01-01T00:00:00.000000Z"},
        {"updated_at", "1970-01-01T00:00:00.000000Z"}
    };
    BOOST_CHECK(json == expected);
    BOOST_CHECK_EQUAL(json["kind"].get<std::string>(), "directory");
    BOOST_CHECK_EQUAL(json["created_at"].get<std::string>(), "1970-01-01T00:00:00.000000Z");
    BOOST_CHECK(json["created_at"] == json["updated_at"]);
    const auto id = json["id"].get<std::string>();
    BOOST_CHECK_EQUAL(response.headers.headers().getSingle("location").value, "/api/v1/nodes/" + id);
    BOOST_CHECK_EQUAL(response.headers.headers().getSingle("etag").value, "\"node-" + id + "-1\"");
    BOOST_CHECK(response.headers.body().empty());
    muduox::net::Buffer bytes;
    BOOST_REQUIRE(response.buffered(false).appendToBuffer(&bytes, muduox::net::HttpRequest::kHttp11, muduox::net::HttpRequest::kPost));
    const auto wire = bytes.retrieveAllAsString();
    BOOST_CHECK(wire.find("HTTP/1.1 201 Created\r\n") == 0);
    BOOST_CHECK(wire.find("Content-Length: " + std::to_string(response.body.size()) + "\r\n") != std::string::npos);
}
BOOST_AUTO_TEST_CASE(invalidSuccessIsNotSerialized) {
    auto value = created();
    value.directory.parentId.reset();
    BOOST_CHECK(!createdDirectoryResponse(value, requestId()));
    value = created(); value.directory.revision = Revision::from(2).value();
    BOOST_CHECK(!createdDirectoryResponse(value, requestId()));
    value = created(); value.directory.name = std::string(256, 'x');
    BOOST_CHECK(!createdDirectoryResponse(value, requestId()));
    value = created(); value.directory.updatedAt += std::chrono::microseconds(1);
    BOOST_CHECK(!createdDirectoryResponse(value, requestId()));
}
BOOST_AUTO_TEST_CASE(errorMappingsAndSerializedStatusLines) {
    struct Case { ProtocolError error; int status; const char* code; const char* reason; };
    const Case cases[] = {
        {ProtocolError::InvalidArgument,400,"INVALID_ARGUMENT","Bad Request"}, {ProtocolError::InvalidName,400,"INVALID_NAME","Bad Request"},
        {ProtocolError::Unauthenticated,401,"UNAUTHENTICATED","Unauthorized"}, {ProtocolError::NotFound,404,"NOT_FOUND","Not Found"},
        {ProtocolError::ParentNotDirectory,409,"PARENT_NOT_DIRECTORY","Conflict"}, {ProtocolError::DepthLimitExceeded,409,"DEPTH_LIMIT_EXCEEDED","Conflict"},
        {ProtocolError::NameConflict,409,"NAME_CONFLICT","Conflict"}, {ProtocolError::QuotaExceeded,403,"QUOTA_EXCEEDED","Forbidden"},
        {ProtocolError::ConcurrentModification,409,"CONCURRENT_MODIFICATION","Conflict"}, {ProtocolError::RequestTooLarge,413,"REQUEST_TOO_LARGE","Content Too Large"},
        {ProtocolError::UnsupportedMediaType,415,"UNSUPPORTED_MEDIA_TYPE","Unsupported Media Type"}, {ProtocolError::RateLimited,429,"RATE_LIMITED","Too Many Requests"},
        {ProtocolError::CommitOutcomeUnknown,500,"COMMIT_OUTCOME_UNKNOWN","Internal Server Error"}, {ProtocolError::InternalError,500,"INTERNAL_ERROR","Internal Server Error"},
        {ProtocolError::TemporarilyUnavailable,503,"TEMPORARILY_UNAVAILABLE","Service Unavailable"}, {ProtocolError::StorageFull,507,"STORAGE_FULL","Insufficient Storage"}};
    for (const auto& entry : cases) {
        auto response = errorResponse(entry.error, requestId());
        const auto json = nlohmann::json::parse(response.body);
        BOOST_CHECK_EQUAL(json.size(), 1);
        BOOST_CHECK_EQUAL(json["error"].size(), 4);
        BOOST_CHECK_EQUAL(json["error"]["code"].get<std::string>(), entry.code);
        BOOST_CHECK(json["error"]["details"].is_object() && json["error"]["details"].empty());
        BOOST_CHECK_EQUAL(json["error"]["request_id"].get<std::string>(), response.headers.headers().getSingle("X-Request-ID").value);
        BOOST_CHECK_EQUAL(response.headers.headers().getSingle("Cache-Control").value, "no-store");
        BOOST_CHECK_EQUAL(response.headers.headers().getSingle("Content-Type").value, "application/json");
        if (entry.status == 401) BOOST_CHECK_EQUAL(response.headers.headers().getSingle("WWW-Authenticate").value, "Bearer realm=\"anof\"");
        else BOOST_CHECK(!response.headers.headers().contains("WWW-Authenticate"));
        muduox::net::Buffer bytes;
        BOOST_REQUIRE(response.buffered(true).appendToBuffer(&bytes, muduox::net::HttpRequest::kHttp11, muduox::net::HttpRequest::kPost));
        BOOST_CHECK(bytes.retrieveAllAsString().find("HTTP/1.1 " + std::to_string(entry.status) + " " + entry.reason + "\r\n") == 0);
    }
    BOOST_CHECK(protocolError(ErrorCode::CommitOutcomeUnknown) == ProtocolError::CommitOutcomeUnknown);
    BOOST_CHECK(protocolError(ErrorCode::IntegrityError) == ProtocolError::InternalError);
    BOOST_CHECK(protocolError(ErrorCode::Retryable) == ProtocolError::TemporarilyUnavailable);
    BOOST_CHECK(protocolError(ErrorCode::StorageFull) == ProtocolError::StorageFull);
}
BOOST_AUTO_TEST_CASE(healthRepresentation) {
    BOOST_CHECK_EQUAL(nlohmann::json::parse(liveResponse(requestId()).body)["status"].get<std::string>(), "live");
    BOOST_CHECK_EQUAL(nlohmann::json::parse(healthResponse(true, requestId()).body)["status"].get<std::string>(), "ready");
    BOOST_CHECK_EQUAL(static_cast<int>(healthResponse(false, requestId()).headers.statusCode()), 503);
}

BOOST_AUTO_TEST_CASE(applicationErrorBoundary) {
    const std::pair<ErrorCode, ProtocolError> cases[] = {
        {ErrorCode::InvalidArgument, ProtocolError::InvalidArgument},
        {ErrorCode::InvalidName, ProtocolError::InvalidName},
        {ErrorCode::Unauthenticated, ProtocolError::Unauthenticated},
        {ErrorCode::NotFound, ProtocolError::NotFound},
        {ErrorCode::ParentNotDirectory, ProtocolError::ParentNotDirectory},
        {ErrorCode::DepthLimitExceeded, ProtocolError::DepthLimitExceeded},
        {ErrorCode::NameConflict, ProtocolError::NameConflict},
        {ErrorCode::QuotaExceeded, ProtocolError::QuotaExceeded},
        {ErrorCode::Conflict, ProtocolError::ConcurrentModification},
        {ErrorCode::Retryable, ProtocolError::TemporarilyUnavailable},
        {ErrorCode::StorageFull, ProtocolError::StorageFull},
        {ErrorCode::CommitOutcomeUnknown, ProtocolError::CommitOutcomeUnknown},
        {ErrorCode::IntegrityError, ProtocolError::InternalError},
        {ErrorCode::InternalError, ProtocolError::InternalError}
    };
    for (const auto& entry : cases) BOOST_CHECK(protocolError(entry.first) == entry.second);
}
