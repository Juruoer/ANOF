#define BOOST_TEST_MODULE HttpDriveIntegration
#include <tests/integration/DownloadFixture.h>
namespace {
std::string url(const Json& n) { return "/api/v1/nodes/" + n.at("id").get<std::string>(); }
std::string trashUrl(const Json& n) { return "/api/v1/trash/" + n.at("id").get<std::string>(); }
std::string match(const Json& n) { return "If-Match: " + n.at("etag").get<std::string>() + "\r\n"; }
Json space(Fixture& f, const std::string& token, const std::string& kind = "private") {
    const auto spaces = f.call("GET", "/api/v1/spaces", 200, nullptr, token);
    for (const auto& s : spaces["items"])
        if (s["kind"] == kind) return s;
    throw std::runtime_error("space missing");
}
Json mkdir(Fixture& f, const std::string& token, const Json& s, const std::string& name, std::string parent = {}) {
    auto created = f.call("POST", "/api/v1/nodes", 201,
        {{"space_id", s["id"]}, {"parent_id", parent.empty() ? s["root_node_id"].get<std::string>() : parent}, {"name", name}}, token);
    return f.call("GET", url(created), 200, nullptr, token);
}
Json nodeView(Fixture& f, const std::string& token, const Json& n) { return f.call("GET", url(n), 200, nullptr, token); }
void code(const Json& j, const char* expected) { BOOST_CHECK_EQUAL(j.at("error").at("code").get<std::string>(), expected); }
std::string rawBody(const std::string& wire) { auto pos=wire.find("\r\n\r\n"); BOOST_REQUIRE(pos!=std::string::npos); return wire.substr(pos+4); }
void content(Fixture& f, const std::string& token, const Json& n, int expected) {
    auto wire=f.exchange(request("GET", url(n)+"/content", nullptr, token));
    BOOST_REQUIRE_MESSAGE(wire.find("HTTP/1.1 "+std::to_string(expected)+" ")==0,wire);
    if(expected==200) BOOST_CHECK_EQUAL(rawBody(wire),"abc");
}
std::int64_t revision(const Json& n) {
    auto tag = n.at("etag").get<std::string>();
    auto start = tag.rfind('-') + 1;
    return std::stoll(tag.substr(start, tag.size() - start - 1));
}
std::string wrongKind(const Json& n, bool trash = false) {
    auto tag = n.at("etag").get<std::string>();
    tag.replace(1, trash ? 5 : 4, trash ? "node" : "trash");
    return "If-Match: " + tag + "\r\n";
}
}
BOOST_AUTO_TEST_CASE(real_file_management_preserves_content_accounting_and_deleted_history) {
    Fixture f; f.start(); auto token=f.login(); auto s=space(f,token);
    auto upload=publish(f,token); auto uploadUrl="/api/v1/uploads/"+upload["id"].get<std::string>();
    BOOST_CHECK(upload.at("result_deleted_at").is_null());
    auto n=f.call("GET","/api/v1/nodes/"+node(upload).toString(),200,nullptr,token);
    auto directory=mkdir(f,token,s,"destination");
    auto unchanged=f.call("PATCH",url(n),200,{{"name",n["name"]}},token,match(n));
    BOOST_CHECK(unchanged["etag"]==n["etag"]);
    auto moved=f.call("PATCH",url(n),200,{{"name","renamed"},{"parent_id",directory["id"]}},token,match(n));
    BOOST_CHECK(moved["id"]==n["id"]);
    code(f.call("PATCH",url(n),412,{{"name","stale"}},token,match(n)),"PRECONDITION_FAILED");
    auto wire=f.exchange(request("POST",url(moved)+"/copy",{{"space_id",s["id"]},{"parent_id",s["root_node_id"]},{"name","copy"}},token,match(moved)));
    BOOST_REQUIRE_MESSAGE(wire.find("HTTP/1.1 201 ")==0,wire);
    auto copy=Json::parse(rawBody(wire)); BOOST_CHECK(wire.find("location: "+url(copy)+"\r\n")!=std::string::npos);
    content(f,token,moved,200); content(f,token,copy,200);
    auto storage=f.call("GET","/api/v1/admin/storage",200,nullptr,token);
    BOOST_CHECK_EQUAL(storage["retained_bytes"].get<std::string>(),"3");
    BOOST_CHECK_EQUAL(storage["used_nodes"].get<std::string>(),"5");
    BOOST_CHECK_EQUAL(storage["reserved_nodes"].get<std::string>(),"0");
    BOOST_CHECK_EQUAL(storage["node_limit"].get<std::string>(),"100000");
    directory=nodeView(f,token,directory);
    auto t=f.call("DELETE",url(directory),200,nullptr,token,match(directory));
    BOOST_CHECK_EQUAL(t["node_count"].get<std::string>(),"2");
    content(f,token,moved,404);
    BOOST_CHECK(f.call("GET",uploadUrl,200,nullptr,token)["result_deleted_at"].is_null());
    f.call("GET",trashUrl(t),200,nullptr,token);
    auto restored=f.call("POST",trashUrl(t)+"/restore",200,Json::object(),token,match(t));
    content(f,token,moved,200);
    t=f.call("DELETE",url(restored),200,nullptr,token,match(restored));
    f.call("DELETE",trashUrl(t),204,nullptr,token,match(t));
    auto history=f.call("GET",uploadUrl,200,nullptr,token);
    BOOST_CHECK(!history["result_deleted_at"].is_null()); BOOST_CHECK(history["result_node_id"]==upload["result_node_id"]);
    auto replay=f.call("POST",uploadUrl+"/complete",200,Json::object(),token);
    BOOST_CHECK(replay["result_deleted_at"]==history["result_deleted_at"]);
    storage=f.call("GET","/api/v1/admin/storage",200,nullptr,token);
    BOOST_CHECK_EQUAL(storage["pending_gc_objects"].get<std::string>(),"0");
    content(f,token,copy,200);
    auto last=f.call("DELETE",url(copy),200,nullptr,token,match(copy));
    f.call("DELETE",trashUrl(last),204,nullptr,token,match(last));
    storage=f.call("GET","/api/v1/admin/storage",200,nullptr,token);
    BOOST_CHECK_EQUAL(storage["pending_gc_bytes"].get<std::string>(),"3");
    BOOST_CHECK_EQUAL(storage["pending_gc_objects"].get<std::string>(),"1");
    BOOST_CHECK_EQUAL(storage["deleting_objects"].get<std::string>(),"0");
    BOOST_CHECK_EQUAL(storage["deleting_bytes"].get<std::string>(),"0");
    BOOST_CHECK_EQUAL(storage["retained_bytes"].get<std::string>(),"3");
    BOOST_CHECK_EQUAL(storage["used_nodes"].get<std::string>(),"2");
    f.locked(); f.stop(); f.start();
    BOOST_CHECK(f.call("GET",uploadUrl,200,nullptr,token)["result_deleted_at"]==history["result_deleted_at"]);
    f.call("GET",trashUrl(last),404,nullptr,token);
    BOOST_CHECK_EQUAL(f.call("GET","/api/v1/admin/storage",200,nullptr,token)["pending_gc_bytes"].get<std::string>(),"3");
}
BOOST_AUTO_TEST_CASE(real_permissions_wrong_kind_conditions_literal_search_and_bound_cursors) {
    Fixture f; f.start(); auto admin=f.login();
    auto invite=f.call("POST","/api/v1/admin/invites",201,{{"max_uses",2}},admin);
    f.call("POST","/api/v1/auth/register",201,registration("alice",invite["token"]));
    f.call("POST","/api/v1/auth/register",201,registration("bob",invite["token"]));
    auto alice=f.login("alice"), bob=f.login("bob"); auto s=space(f,alice), pub=space(f,alice,"public");
    auto a=mkdir(f,alice,s,"literal%_一"), b=mkdir(f,alice,s,"literal%_二");
    auto q="/api/v1/search?space_id="+s["id"].get<std::string>()+"&q=%25_&limit=1";
    auto page=f.call("GET",q,200,nullptr,alice); BOOST_REQUIRE_EQUAL(page["items"].size(),1); BOOST_REQUIRE(!page["next_cursor"].is_null());
    auto cursor=page["next_cursor"].get<std::string>(); auto next=f.call("GET",q+"&cursor="+cursor,200,nullptr,alice);
    BOOST_REQUIRE_EQUAL(next["items"].size(),1); BOOST_CHECK(next["items"][0]["id"]!=page["items"][0]["id"]);
    code(f.call("GET",q+"&cursor="+cursor,400,nullptr,bob),"INVALID_ARGUMENT");
    f.call("GET","/api/v1/search?space_id="+s["id"].get<std::string>()+"&q=other&cursor="+cursor,400,nullptr,alice);
    f.call("GET",q,404,nullptr,bob);
    code(f.call("PATCH",url(a),412,{{"name","wrong"}},alice,wrongKind(a)),"PRECONDITION_FAILED");
    code(f.call("DELETE",url(a),404,nullptr,bob,wrongKind(a)),"NOT_FOUND");
    code(f.call("DELETE",url(a),401,nullptr,{},wrongKind(a)),"UNAUTHENTICATED");
    auto shared=mkdir(f,alice,pub,"mixed"); mkdir(f,bob,pub,"child",shared["id"]); shared=nodeView(f,alice,shared);
    code(f.call("DELETE",url(shared),403,nullptr,alice,wrongKind(shared)),"FORBIDDEN");
    auto mixed=f.call("DELETE",url(shared),200,nullptr,admin,match(shared));
    f.call("GET",trashUrl(mixed),404,nullptr,alice);
    auto t=f.call("DELETE",url(a),200,nullptr,alice,match(a));
    code(f.call("POST",trashUrl(t)+"/restore",412,Json::object(),alice,wrongKind(t,true)),"PRECONDITION_FAILED");
    f.call("DELETE",trashUrl(t),404,nullptr,bob,wrongKind(t,true));
    auto t2=f.call("DELETE",url(b),200,nullptr,alice,match(b));
    auto list="/api/v1/trash?space_id="+s["id"].get<std::string>()+"&limit=1";
    page=f.call("GET",list,200,nullptr,alice); BOOST_REQUIRE(!page["next_cursor"].is_null());
    next=f.call("GET",list+"&cursor="+page["next_cursor"].get<std::string>(),200,nullptr,alice);
    BOOST_REQUIRE_EQUAL(next["items"].size(),1); BOOST_CHECK(next["items"][0]["id"]!=page["items"][0]["id"]);
    auto filtered=f.call("GET",list+"&root_node_id="+a["id"].get<std::string>(),200,nullptr,alice);
    BOOST_REQUIRE_EQUAL(filtered["items"].size(),1); BOOST_CHECK(filtered["items"][0]["id"]==t["id"]);
    BOOST_CHECK(f.call("GET",q,200,nullptr,alice)["items"].empty());
    auto restored=f.call("POST",trashUrl(t)+"/restore",200,{{"name","restored"}},alice,match(t));
    BOOST_CHECK_EQUAL(restored["name"].get<std::string>(),"restored");
    f.call("DELETE",trashUrl(t2),204,nullptr,alice,match(t2));
}

BOOST_AUTO_TEST_CASE(uncertain_drive_commit_is_reported_and_reconciled_after_restart) {
    Fixture f; f.start(); auto token=f.login(); auto s=space(f,token);
    auto n=mkdir(f,token,s,"before"); n=nodeView(f,token,n);
    f.stop(); f.start({},true);
    auto result=f.call("PATCH",url(n),500,{{"name","after"}},token,match(n));
    code(result,"COMMIT_OUTCOME_UNKNOWN"); BOOST_CHECK(result["error"]["details"].empty());
    auto updated=nodeView(f,token,n); BOOST_CHECK_EQUAL(updated["name"].get<std::string>(),"after");
    BOOST_CHECK_EQUAL(revision(updated),revision(n)+1);
    code(f.call("PATCH",url(n),412,{{"name","again"}},token,match(n)),"PRECONDITION_FAILED");
    f.locked(); f.stop(); f.start();
    BOOST_CHECK(nodeView(f,token,n)["etag"]==updated["etag"]);
}
