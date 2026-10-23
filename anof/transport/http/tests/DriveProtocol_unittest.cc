#define BOOST_TEST_MODULE DriveProtocol
#include "DriveTestSupport.h"
#include <nlohmann/json.hpp>
using namespace anof;
using namespace anof::http;
using namespace anof::http::testing;
using Json = nlohmann::json;
namespace {
const auto rid = Identity{}.forExchange(1,1);
DriveCondition condition() { return NodeCondition{driveNode, Revision::from(3).value()}; }
}
BOOST_AUTO_TEST_CASE(conditionsRetainActualKindAndBinding) {
    muduox::net::HttpHeaders h;
    BOOST_CHECK(std::get<ProtocolError>(parseDriveCondition(h)) == ProtocolError::PreconditionRequired);
    for (auto tag : {nodeTag, trashTag}) {
        h.set("If-Match", tag); auto parsed = parseDriveCondition(h); BOOST_REQUIRE(std::holds_alternative<DriveCondition>(parsed));
        auto value = std::get<DriveCondition>(parsed);
        BOOST_CHECK(std::holds_alternative<NodeCondition>(value) == (tag == nodeTag));
        std::visit([&](auto v) { BOOST_CHECK_EQUAL(v.id.toString(), driveNode.toString()); BOOST_CHECK_EQUAL(v.revision.value(), 3); }, value);
    }
    for (auto tag : {std::string("*"), "W/" + nodeTag, nodeTag + "," + nodeTag, "\"node-" + driveNode.toString() + "-0\"", "\"node-" + driveNode.toString() + "-03\"", "\"node-" + driveNode.toString() + "-9223372036854775808\""}) {
        h.set("If-Match", tag); BOOST_CHECK(std::holds_alternative<ProtocolError>(parseDriveCondition(h)));
    }
    h.set("If-Match", nodeTag); h.append("If-Match", nodeTag); BOOST_CHECK(std::holds_alternative<ProtocolError>(parseDriveCondition(h)));
}
BOOST_AUTO_TEST_CASE(strictWriteJsonAndOptionalRestoreTarget) {
    for (auto text : {"{}", "{\"name\":null}", "{\"name\":1}", "{\"name\":\"x\",\"name\":\"y\"}", "{\"other\":\"x\"}", "{\"parent_id\":{}}", "[]", "{\"name\":\"../x\"}"})
        BOOST_CHECK(std::holds_alternative<ProtocolError>(parseUpdateNode(text, driveNode, condition())));
    BOOST_CHECK(std::holds_alternative<UpdateNodeCommand>(parseUpdateNode("{\"name\":\"家庭\"}", driveNode, condition())));
    BOOST_CHECK(std::holds_alternative<ProtocolError>(parseUpdateNode(std::string(65537, ' '), driveNode, condition())));
    auto restored = parseRestoreTrash("{}", driveTrash, condition()); BOOST_REQUIRE(std::holds_alternative<RestoreTrashCommand>(restored));
    BOOST_CHECK(!std::get<RestoreTrashCommand>(restored).parent && !std::get<RestoreTrashCommand>(restored).name);
    BOOST_CHECK(std::holds_alternative<ProtocolError>(parseCopyFile("{}", driveNode, condition())));
    BOOST_CHECK(std::holds_alternative<RestoreTrashCommand>(parseRestoreTrash("{\"name\":\"new\"}", driveTrash, condition())));
}
BOOST_AUTO_TEST_CASE(strictQueriesPreserveLiteralTextAndCursorBindings) {
    const auto prefix = "space_id=" + uploadSpace.toString();
    auto parsed = parseSearchNodes(prefix + "&q=%20A%25_%5C%20"); BOOST_REQUIRE(std::holds_alternative<SearchNodesCommand>(parsed));
    auto query = std::get<SearchNodesCommand>(parsed); BOOST_CHECK_EQUAL(query.text.value(), " A%_\\ "); BOOST_CHECK_EQUAL(query.limit.value(), 100);
    for (auto suffix : {"", "&q=", "&q=x&q=y", "&q=x&limit=201", "&q=x&limit=01", "&q=x&unknown=y", "&q=%00", "&q=%1f", "&q=%C2%80", "&q=%FF", "&q=%xx"})
        BOOST_CHECK(std::holds_alternative<ProtocolError>(parseSearchNodes(prefix + suffix)));
    BOOST_CHECK(std::holds_alternative<ProtocolError>(parseSearchNodes(prefix + "&q=" + std::string(129,'a'))));
    SearchCursor cursor{driveUser, uploadSpace, SearchText::parse("different").value(), {"name", driveNode}};
    auto encoded = encodeCursor(cursor); BOOST_REQUIRE(encoded);
    parsed = parseSearchNodes(prefix + "&q=x&cursor=" + encoded.value()); BOOST_REQUIRE(std::holds_alternative<SearchNodesCommand>(parsed));
    BOOST_CHECK_EQUAL(std::get<SearchNodesCommand>(parsed).after->text.value(), "different");
    for (auto invalid : {encoded.value()+"=", std::string(2049,'a'), std::string("a"), std::string("ab")}) BOOST_CHECK(std::holds_alternative<ProtocolError>(decodeSearchCursor(invalid)));
    TrashCursor trash{driveUser, uploadSpace, driveNode, {UtcTime{}, driveTrash}};
    auto tc = encodeCursor(trash); BOOST_REQUIRE(tc); auto decoded = decodeTrashCursor(tc.value()); BOOST_REQUIRE(std::holds_alternative<TrashCursor>(decoded));
    BOOST_CHECK(std::get<TrashCursor>(decoded).root == driveNode);
    auto listed = parseListTrash(prefix + "&cursor=" + tc.value()); BOOST_REQUIRE(std::holds_alternative<ListTrashCommand>(listed));
    BOOST_CHECK(!std::get<ListTrashCommand>(listed).root); BOOST_CHECK(std::get<ListTrashCommand>(listed).after->root == driveNode);
    BOOST_CHECK(std::holds_alternative<ProtocolError>(parseListTrash(prefix + "&q=x")));
    BOOST_CHECK(std::holds_alternative<ProtocolError>(decodeSearchCursor(tc.value())));
}
BOOST_AUTO_TEST_CASE(trashWireHasExactPublicFieldsAndPageHeaders) {
    auto value = driveTrashView(); auto response = trashResponse(value,rid); BOOST_REQUIRE(response); auto json = Json::parse(response.value().body);
    BOOST_CHECK_EQUAL(json.size(),13); BOOST_CHECK_EQUAL(json["node_count"],"2"); BOOST_CHECK_EQUAL(json["logical_bytes"],"123");
    BOOST_CHECK_EQUAL(json["original_parent_id"],uploadParent.toString()); BOOST_CHECK_EQUAL(json["etag"],trashTag);
    for (auto key : {"deleted_by","sole_creator_id","blob_id","members"}) BOOST_CHECK(!json.contains(key));
    Page<TrashView, TrashCursor> page{{value}, TrashCursor{driveUser, uploadSpace, driveNode, {UtcTime{}, driveTrash}}};
    auto output = trashPageResponse(page,rid); BOOST_REQUIRE(output); BOOST_CHECK_EQUAL(Json::parse(output.value().body)["items"].size(),1);
    page.next->position.deletedAt += std::chrono::microseconds(1); BOOST_CHECK(!trashPageResponse(page,rid));
    value.entry.expiresAt = UtcTime{}; BOOST_CHECK(!trashResponse(value,rid));
    Page<NodeDetails,SearchCursor> nodes{{driveNodeView()}, SearchCursor{driveUser,uploadSpace,SearchText::parse("x").value(),{"家庭",driveNode}}};
    BOOST_CHECK(searchResponse(nodes,rid)); nodes.next->position.name="other"; BOOST_CHECK(!searchResponse(nodes,rid));
}
BOOST_AUTO_TEST_CASE(uploadHistoryAndStorageDerivedFields) {
    auto upload=uploadView(); auto output=uploadResponse(upload,rid); BOOST_REQUIRE(output); BOOST_CHECK(Json::parse(output.value().body)["result_deleted_at"].is_null());
    upload.resultDeletedAt=UtcTime{}; BOOST_CHECK(!uploadResponse(upload,rid));
    upload.state=UploadState::Completed; upload.terminalAt=UtcTime{}; upload.actualSha256=Sha256::parse(std::string(64,'a')).value(); upload.resultNodeId=driveNode;
    output=uploadResponse(upload,rid); BOOST_REQUIRE(output); auto json=Json::parse(output.value().body); BOOST_CHECK_EQUAL(json["result_node_id"],driveNode.toString()); BOOST_CHECK_EQUAL(json["result_deleted_at"],"1970-01-01T00:00:00.000000Z");
    auto created=uploadCreatedResponse({upload,UploadCreateDisposition::Existing},rid); BOOST_REQUIRE(created); BOOST_CHECK_EQUAL(Json::parse(created.value().body)["result_deleted_at"],json["result_deleted_at"]);
    auto completed=uploadCompleteResponse({upload,UploadCompleteDisposition::Completed},rid); BOOST_REQUIRE(completed); BOOST_CHECK_EQUAL(Json::parse(completed.value().body)["result_deleted_at"],json["result_deleted_at"]);
    auto listed=uploadsResponse({{upload},{}},rid); BOOST_REQUIRE(listed); BOOST_CHECK_EQUAL(Json::parse(listed.value().body)["items"][0]["result_deleted_at"],json["result_deleted_at"]);
    auto head=uploadHeadResponse(upload,rid); BOOST_REQUIRE(head); BOOST_CHECK(head.value().body.empty()); BOOST_CHECK(head.value().omitLength);
    upload.resultDeletedAt=UtcTime{}-std::chrono::microseconds(1); BOOST_CHECK(!uploadResponse(upload,rid));
    auto storage=storageView(); storage.accounting.retained=ByteCount::from(100).value();
    storage.gc={ByteCount::from(80).value(),NodeCount::from(3).value(),ByteCount::from(20).value(),NodeCount::from(1).value()};
    storage.nodes={NodeCount::from(5).value(),NodeCount::from(2).value(),Revision::from(1).value()};
    auto view=storageResponse(storage,rid); BOOST_REQUIRE(view); json=Json::parse(view.value().body);
    BOOST_CHECK_EQUAL(json.size(),15); BOOST_CHECK_EQUAL(json["pending_gc_bytes"],"80"); BOOST_CHECK_EQUAL(json["pending_gc_objects"],"3"); BOOST_CHECK_EQUAL(json["deleting_bytes"],"20"); BOOST_CHECK_EQUAL(json["deleting_objects"],"1"); BOOST_CHECK_EQUAL(json["used_nodes"],"5"); BOOST_CHECK_EQUAL(json["reserved_nodes"],"2"); BOOST_CHECK_EQUAL(json["node_limit"],"100000");
    storage.gc.deletingBytes=ByteCount::from(81).value(); BOOST_CHECK(!storageResponse(storage,rid));
}
BOOST_AUTO_TEST_CASE(newErrorsHaveStableCodesAndOperationMappingsRemainDistinct) {
    const std::pair<ErrorCode,const char*> errors[]={{ErrorCode::CrossSpaceMove,"CROSS_SPACE_MOVE"},{ErrorCode::DirectoryCycle,"DIRECTORY_CYCLE"},{ErrorCode::TreeLimitExceeded,"TREE_LIMIT_EXCEEDED"},{ErrorCode::NodeLimitReached,"NODE_LIMIT_REACHED"},{ErrorCode::RestoreTargetUnavailable,"RESTORE_TARGET_UNAVAILABLE"},{ErrorCode::QueryLimitExceeded,"QUERY_LIMIT_EXCEEDED"}};
    for (auto [code,text]:errors) {auto response=errorResponse(protocolError(code),rid); BOOST_CHECK_EQUAL(Json::parse(response.body)["error"]["code"],text); BOOST_CHECK_EQUAL(static_cast<int>(response.headers.statusCode()),code==ErrorCode::QueryLimitExceeded?503:409); BOOST_CHECK(Json::parse(response.body)["error"]["details"].empty());}
    BOOST_CHECK_EQUAL(static_cast<int>(errorResponse(protocolError(ErrorCode::StorageFull),rid).headers.statusCode()),507);
    BOOST_CHECK_EQUAL(static_cast<int>(errorResponse(transferError(ErrorCode::StorageFull),rid).headers.statusCode()),503);
}

BOOST_AUTO_TEST_CASE(cursorRejectsWrongSortDuplicateUnknownKeysAndNoncanonicalTail) {
    // 用测试侧 base64url 编码构造不符合游标结构的输入，不借生产解码器伪造校验结果。
    auto encode=[](const std::string& text) {
        const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        unsigned bits=0,count=0;std::string output;
        for(unsigned char byte:text) {bits=(bits<<8)|byte;count+=8;while(count>=6){count-=6;output+=alphabet[(bits>>count)&63];}}
        if (count) output += alphabet[(bits << (6-count)) & 63];
        return output;
    };
    Json value{{"v",1},{"kind","search"},{"sort","name-id-v1"},{"user_id",driveUser.toString()},{"space_id",uploadSpace.toString()},{"q","x"},{"after_name","a"},{"after_id",driveNode.toString()}};
    BOOST_REQUIRE(std::holds_alternative<SearchCursor>(decodeSearchCursor(encode(value.dump()))));
    auto bad=value;bad["sort"]="name-id-v2";BOOST_CHECK(std::holds_alternative<ProtocolError>(decodeSearchCursor(encode(bad.dump()))));
    bad=value;bad["v"]=2;BOOST_CHECK(std::holds_alternative<ProtocolError>(decodeSearchCursor(encode(bad.dump()))));
    bad=value;bad["unknown"]=1;BOOST_CHECK(std::holds_alternative<ProtocolError>(decodeSearchCursor(encode(bad.dump()))));
    auto duplicate=value.dump();duplicate.insert(1,"\"v\":1,");BOOST_CHECK(std::holds_alternative<ProtocolError>(decodeSearchCursor(encode(duplicate))));
    bad=value;bad["after_id"]="00000000-0000-0000-0000-000000000000";BOOST_CHECK(std::holds_alternative<ProtocolError>(decodeSearchCursor(encode(bad.dump()))));
    bad=value;bad["q"]=std::string(129,'a');BOOST_CHECK(std::holds_alternative<ProtocolError>(decodeSearchCursor(encode(bad.dump()))));
    auto trash=encodeCursor(TrashCursor{driveUser,uploadSpace,{}, {UtcTime{},driveTrash}});BOOST_REQUIRE(trash);auto result=decodeTrashCursor(trash.value());BOOST_REQUIRE(std::holds_alternative<TrashCursor>(result));BOOST_CHECK(!std::get<TrashCursor>(result).root);
}
