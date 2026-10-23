#define BOOST_TEST_MODULE DriveClient
#include <anof/client/DriveClient.h>
#include <anof/client/TransferProtocol.h>
#include <anof/client/tests/Fixtures.h>
#include <boost/test/unit_test.hpp>
using namespace anof;
using namespace anof::client;
using namespace anof::client::test;
namespace {
Json trashJson() {return {{"id",rid},{"instance_id",sid},{"space_id",sid},{"root_node_id",nid},{"original_parent_id",uid},{"name","照片"},{"kind","directory"},{"deleted_at",timestamp},{"expires_at","2026-11-10T10:00:00.000000Z"},{"revision","1"},{"etag","\"trash-"+rid+"-1\""},{"node_count","1"},{"logical_bytes","0"}};}
HttpResponse nodeResponse() {auto r=response(200,nodeJson());r.headers["etag"]=nodeJson()["etag"];return r;}
HttpResponse trashResponse() {auto r=response(200,trashJson());r.headers["etag"]=trashJson()["etag"];return r;}
struct Fixture {
    Temp temp;ProfileStore store{temp.config()};MockTransport http;std::optional<LockedProfile> held;
    Fixture(){BOOST_REQUIRE(store.add("admin",Origin::parse("http://127.0.0.1:8080",true).value()));auto lock=store.lock("admin");BOOST_REQUIRE(lock);held.emplace(std::move(lock).value());BOOST_REQUIRE(held->saveCredential(credential()));}
    DriveClient api(){return {http,*held};}
};
}
BOOST_AUTO_TEST_CASE(strict_conditions_and_representations) {
    BOOST_CHECK(driveCondition("\"trash-"+nid+"-1\""));
    for(const auto& bad:{"*","W/\"node-x-1\"","\"node-x-1\""})BOOST_CHECK(!driveCondition(bad));
    BOOST_CHECK(!driveCondition("\"user-"+uid+"-1\""));
    auto r=nodeResponse();BOOST_CHECK(decodeDriveResponse(DriveOperation::Update,r));
    r.headers["content-type"]="Application/JSON; charset=\"UTF-8\"";BOOST_CHECK(decodeDriveResponse(DriveOperation::Update,r));
    r.headers["content-type"]="application/json; charset=gbk";BOOST_CHECK(!decodeDriveResponse(DriveOperation::Update,r));
    r.headers["content-type"]="application/json";
    r.headers["etag"]="\"node-"+nid+"-2\"";
    auto fail=decodeDriveResponse(DriveOperation::Update,r);BOOST_REQUIRE(!fail);BOOST_CHECK_EQUAL(fail.error().exitCode(),6);
    auto t=trashResponse();BOOST_CHECK(decodeDriveResponse(DriveOperation::ShowTrash,t));
    for(const auto& key:{"expires_at","revision","root_node_id","node_count","instance_id"}) {
        auto j=trashJson();j.erase(key);auto bad=response(200,j);bad.headers["etag"]=trashJson()["etag"];
        BOOST_CHECK(!decodeDriveResponse(DriveOperation::ShowTrash,bad));
    }
    for(const auto& change:{Json{{"expires_at",timestamp}},Json{{"revision","2"}},Json{{"node_count","0"}},Json{{"logical_bytes",1}},Json{{"unexpected",true}}}) {
        auto j=trashJson();j.update(change);auto bad=response(200,j);bad.headers["etag"]=trashJson()["etag"];
        BOOST_CHECK(!decodeDriveResponse(DriveOperation::ShowTrash,bad));
    }
    auto duplicate=rawResponse(200,"{\"items\":[],\"items\":[],\"next_cursor\":null}");BOOST_CHECK(!decodeDriveResponse(DriveOperation::Search,duplicate));
    auto empty=response(200,{{"items",Json::array()},{"next_cursor","abc"}});BOOST_CHECK(!decodeDriveResponse(DriveOperation::ListTrash,empty));
    auto purge=rawResponse(204,"");purge.headers.erase("content-type");BOOST_CHECK(decodeDriveResponse(DriveOperation::Purge,purge));purge.body.append("x");BOOST_CHECK(!decodeDriveResponse(DriveOperation::Purge,purge));
}
BOOST_AUTO_TEST_CASE(eight_methods_are_single_requests_and_validate_bindings) {
    Fixture f;auto api=f.api();auto n=NodeId::parse(nid).value();auto s=SpaceId::parse(sid).value();auto t=TrashId::parse(rid).value();auto tag=driveCondition("\"trash-"+nid+"-1\"").value();
    unsigned mode=0;
    f.http.reply=[&](const auto&,const HttpRequest& req)->ClientResult<HttpResponse>{
        BOOST_REQUIRE(req.bearer);BOOST_CHECK(req.path.find("/api/v1/")==0);
        if(mode==0){BOOST_CHECK(req.method==Method::Patch);BOOST_CHECK_EQUAL(req.ifMatch->value(),tag.value());BOOST_CHECK_EQUAL(req.path,"/api/v1/nodes/"+nid);return nodeResponse();}
        if(mode==1){BOOST_CHECK(req.method==Method::Post);auto j=nodeJson();j["id"]=uid;j["etag"]="\"node-"+uid+"-1\"";j["kind"]="file";j["size"]="0";j["sha256"]=std::string(64,'a');auto r=response(201,j);r.headers["etag"]=j["etag"];r.headers["location"]="/api/v1/nodes/"+uid;return r;}
        if(mode==2){BOOST_CHECK(req.method==Method::Delete);auto r=trashResponse();r.headers["location"]="/api/v1/trash/"+rid;return r;}
        if(mode==3){BOOST_CHECK(req.method==Method::Get);BOOST_CHECK(req.path.find("q=%25_%20")!=std::string::npos);return response(200,{{"items",Json::array()},{"next_cursor",nullptr}});}
        if(mode==4){BOOST_CHECK(req.path.find("root_node_id="+nid)!=std::string::npos);return response(200,{{"items",Json::array({trashJson()})},{"next_cursor",nullptr}});}
        if(mode==5){BOOST_CHECK_EQUAL(req.path,"/api/v1/trash/"+rid);return trashResponse();}
        if(mode==6){BOOST_CHECK_EQUAL(req.body.view(),"{}");auto r=nodeResponse();r.headers["location"]="/api/v1/nodes/"+nid;return r;}
        BOOST_CHECK(req.method==Method::Delete);auto r=rawResponse(204,"");r.headers.erase("content-type");return r;
    };
    BOOST_REQUIRE(api.update(n,{},"照片",tag));mode=1;BOOST_REQUIRE(api.copy(n,s,n,"照片",tag));mode=2;BOOST_REQUIRE(api.remove(n,tag));mode=3;BOOST_REQUIRE(api.search(s,"%_ "));mode=4;BOOST_REQUIRE(api.listTrash(s,n));mode=5;BOOST_REQUIRE(api.showTrash(t));mode=6;BOOST_REQUIRE(api.restore(t,{},{},tag));mode=7;BOOST_REQUIRE(api.purge(t,tag));BOOST_CHECK_EQUAL(f.http.calls,8);
    BOOST_CHECK(!api.update(n,{},{},tag));BOOST_CHECK(!api.search(s,""));BOOST_CHECK_EQUAL(f.http.calls,8);
    f.http.reply=[](const auto&,const auto&)->ClientResult<HttpResponse>{auto r=nodeResponse();auto j=nodeJson();j["id"]=uid;j["etag"]="\"node-"+uid+"-1\"";r=response(200,j);r.headers["etag"]=j["etag"];return r;};
    auto mismatch=api.update(n,{},"照片",tag);BOOST_REQUIRE(!mismatch);BOOST_CHECK_EQUAL(mismatch.error().exitCode(),6);
}
BOOST_AUTO_TEST_CASE(rejections_unknown_and_transport_exceptions_never_retry) {
    Fixture f;auto api=f.api();auto n=NodeId::parse(nid).value();auto tag=driveCondition("\"node-"+nid+"-1\"").value();
    for(const auto& item:std::vector<std::pair<int,std::string>>{{409,"CROSS_SPACE_MOVE"},{409,"DIRECTORY_CYCLE"},{409,"TREE_LIMIT_EXCEEDED"},{409,"NODE_LIMIT_REACHED"},{409,"RESTORE_TARGET_UNAVAILABLE"},{503,"QUERY_LIMIT_EXCEEDED"},{500,"COMMIT_OUTCOME_UNKNOWN"},{412,"PRECONDITION_FAILED"},{507,"STORAGE_FULL"}}) {
        f.http.reply=[&](const auto&,const auto&)->ClientResult<HttpResponse>{return rejection(item.first,item.second);};auto before=f.http.calls;auto r=api.remove(n,tag);BOOST_REQUIRE(!r);BOOST_CHECK_EQUAL(f.http.calls,before+1);BOOST_CHECK_EQUAL(r.error().code,item.second);BOOST_CHECK_EQUAL(r.error().exitCode(),item.second=="COMMIT_OUTCOME_UNKNOWN"?6:3);
    }
    f.http.reply=[](const auto&,const auto&)->ClientResult<HttpResponse>{throw std::runtime_error("not trusted");};auto before=f.http.calls;auto r=api.remove(n,tag);BOOST_REQUIRE(!r);BOOST_CHECK_EQUAL(r.error().exitCode(),6);BOOST_CHECK_EQUAL(f.http.calls,before+1);
    auto thrownRead=api.search(SpaceId::parse(sid).value(),"a");BOOST_REQUIRE(!thrownRead);BOOST_CHECK_EQUAL(thrownRead.error().exitCode(),4);BOOST_CHECK(thrownRead.error().remote==RemoteOutcome::Unknown);
    f.http.reply=[](const auto&,const auto&)->ClientResult<HttpResponse>{return rawResponse(200,"broken");};auto read=api.search(SpaceId::parse(sid).value(),"a");BOOST_REQUIRE(!read);BOOST_CHECK_EQUAL(read.error().exitCode(),4);
}
BOOST_AUTO_TEST_CASE(storage_gc_and_node_counters_are_required_and_checked) {
    Json j{{"instance_id",sid},{"content_budget_bytes","10"},{"retained_bytes","3"},{"reserved_bytes","0"},{"pending_gc_bytes","3"},{"pending_gc_objects","2"},{"deleting_bytes","0"},{"deleting_objects","1"},{"used_nodes","2"},{"reserved_nodes","0"},{"node_limit","100000"},{"disk_status","UNAVAILABLE"},{"free_bytes",nullptr},{"safety_margin_bytes",nullptr},{"sampled_at",nullptr}};
    auto parse=[&](Json value){auto r=response(200,value);r.headers["etag"]="\"storage-"+sid+"-1\"";return decodeTransfer(TransferOperation::Storage,r);};
    BOOST_REQUIRE(parse(j));
    for(const auto& key:{"pending_gc_bytes","pending_gc_objects","deleting_bytes","deleting_objects","used_nodes","reserved_nodes","node_limit"}){auto v=j;v.erase(key);BOOST_CHECK(!parse(v));}
    for(const auto& change:{Json{{"pending_gc_bytes","4"}},Json{{"deleting_objects","3"}},Json{{"used_nodes","100000"},{"reserved_nodes","1"}},Json{{"node_limit","100001"}}}){auto v=j;v.update(change);BOOST_CHECK(!parse(v));}
}
