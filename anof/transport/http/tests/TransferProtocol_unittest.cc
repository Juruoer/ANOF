#define BOOST_TEST_MODULE TransferProtocol
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/TransferRequest.h>
#include <anof/transport/http/TransferResponse.h>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "UploadTestSupport.h"
using namespace anof;
using namespace anof::http;
using namespace anof::http::testing;
using Json = nlohmann::json;
namespace {
const auto rid = Identity{}.forExchange(1, 1);
template <typename T>
void invalid(const Parsed<T>& value, ProtocolError code = ProtocolError::InvalidArgument) {
    BOOST_REQUIRE(std::holds_alternative<ProtocolError>(value));
    BOOST_CHECK(std::get<ProtocolError>(value) == code);
}
muduox::net::HttpHeaders patchHeaders() {
    muduox::net::HttpHeaders h;
    h.append("Content-Length", "1");
    h.append("Upload-Offset", "0");
    h.append("Content-Type", "application/offset+octet-stream");
    return h;
}
}  // namespace
BOOST_AUTO_TEST_CASE(createAndCompleteStrictOwnedInputs) {
    auto parsed = parseCreateUpload(createUploadJson());
    BOOST_REQUIRE(std::holds_alternative<CreateUploadCommand>(parsed));
    BOOST_CHECK(std::get<CreateUploadCommand>(parsed).clientUploadId == operationId);
    for (auto body : { "", "[]", "{}", "{\"name\":\"a\",\"name\":\"b\"}", "{\"parent_id\":null}" }) invalid(parseCreateUpload(body));
    auto json = Json::parse(createUploadJson());
    for (auto length : { "00", "-1", "1099511627777", "9223372036854775808", "1e2", " 1" }) {
        auto bad = json;
        bad["length"] = length;
        invalid(parseCreateUpload(bad.dump()));
    }
    json["length"] = "1099511627776";
    BOOST_CHECK(std::holds_alternative<CreateUploadCommand>(parseCreateUpload(json.dump())));
    json["sha256"] = std::string(64, 'A');
    invalid(parseCreateUpload(json.dump()));
    json["sha256"] = std::string(64, 'a');
    BOOST_CHECK(std::holds_alternative<CreateUploadCommand>(parseCreateUpload(json.dump())));
    json["extra"] = 1;
    invalid(parseCreateUpload(json.dump()));
    invalid(parseCompleteUpload("", uploadId));
    invalid(parseCompleteUpload("{\"name\":\"x\"}", uploadId));
    BOOST_CHECK(!std::get<CompleteUploadCommand>(parseCompleteUpload("{}", uploadId)).target);
    const auto target = "{\"parent_id\":\"" + uploadParent.toString() + "\",\"name\":\"新文件\"}";
    BOOST_CHECK(std::get<CompleteUploadCommand>(parseCompleteUpload(target, uploadId)).target->name == "新文件");
    invalid(parseCreateUpload(std::string(65537, 'x')), ProtocolError::RequestTooLarge);
    invalid(parseCompleteUpload("{\"name\":{}}", uploadId));
    invalid(parseUploadId("60000000-0000-4000-8000-00000000000A"));
}
BOOST_AUTO_TEST_CASE(storageStrongConditionsAndNumericLimits) {
    muduox::net::HttpHeaders h;
    invalid(parseStorageCondition(h), ProtocolError::PreconditionRequired);
    const auto tag = "\"storage-" + instanceId.toString() + "-9223372036854775807\"";
    h.append("If-Match", " \t" + tag + "\t ");
    auto c = parseStorageCondition(h);
    BOOST_REQUIRE(std::holds_alternative<StorageCondition>(c));
    BOOST_CHECK(std::get<StorageCondition>(c).instance == instanceId);
    for (const auto& bad : { std::string(""), std::string("*"), "W/" + tag, tag + ", " + tag, "\"storage-" + instanceId.toString() + "-0\"", "\"storage-" + instanceId.toString() + "-01\"" }) {
        muduox::net::HttpHeaders b;
        b.append("If-Match", bad);
        invalid(parseStorageCondition(b));
    }
    h.append("If-Match", tag);
    invalid(parseStorageCondition(h));
    auto condition = std::get<StorageCondition>(c);
    for (auto number : { "0", "9223372036854775807" }) BOOST_CHECK(std::holds_alternative<SetStorageBudgetCommand>(parseStorageUpdate(std::string("{\"content_budget_bytes\":\"") + number + "\"}", condition)));
    for (auto body : { "{}", "{\"content_budget_bytes\":1}", "{\"content_budget_bytes\":\"-1\"}", "{\"content_budget_bytes\":\"0\",\"content_budget_bytes\":\"1\"}" }) invalid(parseStorageUpdate(body, condition));
}
BOOST_AUTO_TEST_CASE(patchHeaderClassification) {
    auto h = patchHeaders();
    BOOST_CHECK(std::get<PreparePatchCommand>(parsePatchHeaders(h, uploadId)).length.value() == 1);
    muduox::net::HttpHeaders empty;
    invalid(parsePatchHeaders(empty, uploadId), ProtocolError::LengthRequired);
    empty.append("Transfer-Encoding", "chunked");
    invalid(parsePatchHeaders(empty, uploadId), ProtocolError::UnsupportedMediaType);
    h.append("Content-Length", "1");
    invalid(parsePatchHeaders(h, uploadId));
    for (const auto& item : { std::pair{ "0", ProtocolError::InvalidArgument }, { "8388609", ProtocolError::RequestTooLarge }, { "-1", ProtocolError::InvalidArgument } }) {
        muduox::net::HttpHeaders v;
        v.append("Content-Length", item.first);
        v.append("Upload-Offset", "0");
        v.append("Content-Type", "application/offset+octet-stream");
        invalid(parsePatchHeaders(v, uploadId), item.second);
    }
    for (auto value : { "00", "-1", "9223372036854775808", "1,2" }) {
        muduox::net::HttpHeaders v;
        v.append("Content-Length", "1");
        v.append("Upload-Offset", value);
        v.append("Content-Type", "application/offset+octet-stream");
        invalid(parsePatchHeaders(v, uploadId));
    }
    h = patchHeaders();
    h.append("Trailer", "x");
    invalid(parsePatchHeaders(h, uploadId));
    h = patchHeaders();
    h.append("Content-Encoding", "gzip");
    invalid(parsePatchHeaders(h, uploadId), ProtocolError::UnsupportedMediaType);
}
BOOST_AUTO_TEST_CASE(uploadCursorRoundTripAndQueryIsolation) {
    auto user = UserId::parse("40000000-0000-4000-8000-000000000001").value();
    UploadCursor c{ user, { UtcTime{}, uploadId } };
    auto encoded = encodeCursor(c);
    BOOST_REQUIRE(encoded);
    auto decoded = decodeUploadCursor(encoded.value());
    BOOST_REQUIRE(std::holds_alternative<UploadCursor>(decoded));
    BOOST_CHECK(std::get<UploadCursor>(decoded).userId == user);
    auto query = parseListUploads("?limit=200&cursor=" + encoded.value());
    BOOST_REQUIRE(std::holds_alternative<ListUploadsCommand>(query));
    BOOST_CHECK(std::get<ListUploadsCommand>(query).size.value() == 200);
    for (auto q : { "?limit=0", "?limit=201", "?limit=01", "?state=OPEN", "?limit=1&limit=1", "?cursor=bad", "?client_upload_id=bad" }) invalid(parseListUploads(q));
    invalid(parseListUploads("?client_upload_id=" + operationId.toString() + "&cursor=" + encoded.value()));
    BOOST_CHECK(std::get<ListUploadsCommand>(parseListUploads("?client_upload_id=" + operationId.toString())).clientUploadId == operationId);
    invalid(decodeUploadCursor(encoded.value() + "="));
    invalid(decodeUploadCursor(std::string(2049, 'a')));
}
BOOST_AUTO_TEST_CASE(uploadDtoNullsStatesAndDispositions) {
    auto u = uploadView();
    auto wire = uploadResponse(u, rid);
    BOOST_REQUIRE(wire);
    auto json = Json::parse(wire.value().body);
    BOOST_CHECK(json["terminal_at"].is_null());
    BOOST_CHECK(json["expected_sha256"].is_null());
    BOOST_CHECK_EQUAL(json["max_patch_bytes"], 8388608);
    BOOST_CHECK(!json.contains("generation"));
    BOOST_CHECK(!json.contains("cancel_requested"));
    for (auto d : { UploadCreateDisposition::Created, UploadCreateDisposition::Existing, UploadCreateDisposition::Creating }) {
        auto out = uploadCreatedResponse({ u, d }, rid);
        BOOST_REQUIRE(out);
        BOOST_CHECK_EQUAL(static_cast<int>(out.value().headers.statusCode()), d == UploadCreateDisposition::Created ? 201 : d == UploadCreateDisposition::Creating ? 202
                                                                                                                                                                   : 200);
    }
    u.state = UploadState::Completed;
    BOOST_CHECK(!uploadResponse(u, rid));
    u.terminalAt = UtcTime{};
    u.resultNodeId = uploadParent;
    u.actualSha256 = Sha256::parse(std::string(64, 'a')).value();
    BOOST_CHECK(uploadResponse(u, rid));
    auto complete = uploadCompleteResponse({ u, UploadCompleteDisposition::Completed }, rid);
    BOOST_CHECK_EQUAL(static_cast<int>(complete.value().headers.statusCode()), 200);
    u.offset = UploadOffset::from(200001).value();
    BOOST_CHECK(!uploadResponse(u, rid));
}
BOOST_AUTO_TEST_CASE(storageSampleAndOperationErrors) {
    auto s = storageView();
    auto out = storageResponse(s, rid);
    BOOST_REQUIRE(out);
    auto json = Json::parse(out.value().body);
    BOOST_CHECK(json["free_bytes"].is_null());
    BOOST_CHECK_EQUAL(json["disk_status"], "UNAVAILABLE");
    BOOST_CHECK(!json.contains("revision"));
    s.disk = DiskSample{ ByteCount::from(10).value(), ByteCount::from(1).value(), UtcTime{} };
    json = Json::parse(storageResponse(s, rid).value().body);
    BOOST_CHECK_EQUAL(json["free_bytes"], "10");
    BOOST_CHECK_EQUAL(static_cast<int>(errorResponse(transferError(ErrorCode::StorageFull), rid).headers.statusCode()), 503);
    BOOST_CHECK_EQUAL(static_cast<int>(errorResponse(protocolError(ErrorCode::StorageFull), rid).headers.statusCode()), 507);
    for (auto code : { ErrorCode::UploadKeyConflict, ErrorCode::UploadOffsetMismatch, ErrorCode::UploadLengthMismatch, ErrorCode::UploadBusy, ErrorCode::UploadIncomplete, ErrorCode::UploadStateConflict, ErrorCode::UploadCommitInProgress, ErrorCode::StorageBudgetExceeded, ErrorCode::NotAFile }) {
        auto error = errorResponse(transferError(code), rid);
        BOOST_CHECK_EQUAL(static_cast<int>(error.headers.statusCode()), 409);
        BOOST_CHECK(Json::parse(error.body)["error"]["details"].empty());
    }
    auto conflict = patchConflictResponse({ UploadOffset::from(42).value() }, rid);
    BOOST_CHECK_EQUAL(Json::parse(conflict.body)["error"]["details"]["offset"], "42");
    BOOST_CHECK_EQUAL(static_cast<int>(errorResponse(transferError(ErrorCode::UploadTerminated), rid).headers.statusCode()), 410);
    BOOST_CHECK_EQUAL(static_cast<int>(errorResponse(transferError(ErrorCode::UploadLimitReached), rid).headers.statusCode()), 429);
}
BOOST_AUTO_TEST_CASE(uploadPageBoundariesAndCursorBindings) {
    const auto user = UserId::parse("40000000-0000-4000-8000-000000000001").value();
    Page<UploadView, UploadCursor> page{ { uploadView() }, UploadCursor{ user, { UtcTime{}, uploadId } } };
    auto wire = uploadsResponse(page, rid);
    BOOST_REQUIRE(wire);
    auto cursor = decodeUploadCursor(Json::parse(wire.value().body)["next_cursor"].get<std::string>());
    BOOST_CHECK(std::get<UploadCursor>(cursor).userId == user);
    page.items.clear();
    BOOST_CHECK(!uploadsResponse(page, rid));
    page.next.reset();
    BOOST_CHECK(uploadsResponse(page, rid));
    page.items.assign(200, uploadView());
    BOOST_CHECK(uploadsResponse(page, rid));
    page.items.push_back(uploadView());
    BOOST_CHECK(!uploadsResponse(page, rid));
    const auto wrong = encodeCursor(UserCursor{ user, { user } });
    BOOST_REQUIRE(wrong);
    invalid(decodeUploadCursor(wrong.value()));
}
BOOST_AUTO_TEST_CASE(decodedDuplicateFieldsAndCanonicalIdentifiersAreRejected) {
    const auto base = createUploadJson();
    auto duplicate = base;
    duplicate.insert(duplicate.size() - 1, ",\"na\\u006de\":\"x\"");
    invalid(parseCreateUpload(duplicate));
    auto json = Json::parse(base);
    for (auto key : { "client_upload_id", "space_id", "parent_id" }) {
        auto bad = json;
        bad[key] = "ABCDEF00-0000-4000-8000-000000000001";
        invalid(parseCreateUpload(bad.dump()));
        bad[key] = "00000000-0000-0000-0000-000000000000";
        invalid(parseCreateUpload(bad.dump()));
    }
    json["name"] = "../x";
    invalid(parseCreateUpload(json.dump()), ProtocolError::InvalidName);
    json["name"] = std::string(256, 'x');
    invalid(parseCreateUpload(json.dump()), ProtocolError::InvalidName);
    for (auto value : { Json(0), Json(nullptr), Json(true), Json::array(), Json::object() }) {
        auto bad = Json::parse(base);
        bad["length"] = value;
        invalid(parseCreateUpload(bad.dump()));
    }
    invalid(parseCreateUpload(base + "{}"));
}
BOOST_AUTO_TEST_CASE(terminalFailureAndHeaderOnlyDtoValidateWireInvariants) {
    auto u = uploadView();
    auto head = uploadHeadResponse(u, rid);
    BOOST_REQUIRE(head);
    BOOST_CHECK(head.value().omitLength);
    BOOST_CHECK(head.value().body.empty());
    for (auto state : { UploadState::Completed, UploadState::Cancelled, UploadState::Expired, UploadState::Failed }) {
        u = uploadView();
        u.state = state;
        BOOST_CHECK(!uploadResponse(u, rid));
        u.terminalAt = UtcTime{};
        if (state == UploadState::Completed) {
            u.resultNodeId = uploadParent;
            u.actualSha256 = Sha256::parse(std::string(64, 'a')).value();
        }
        u.lastError = TransferFailure::ChecksumMismatch;
        auto out = uploadResponse(u, rid);
        BOOST_REQUIRE(out);
        BOOST_CHECK_EQUAL(Json::parse(out.value().body)["last_error"], "CHECKSUM_MISMATCH");
    }
    u = uploadView();
    u.state = static_cast<UploadState>(100);
    BOOST_CHECK(!uploadResponse(u, rid));
    u = uploadView();
    u.state = UploadState::Ready;
    BOOST_CHECK(!uploadResponse(u, rid));
}
