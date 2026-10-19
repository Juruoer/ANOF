#define BOOST_TEST_MODULE AnofClientProtocol
#include <boost/test/unit_test.hpp>
#include <anof/client/tests/Fixtures.h>
#include <anof/client/internal/Json.h>
using namespace anof;
using namespace anof::client;
using namespace anof::client::test;

BOOST_AUTO_TEST_CASE(origin_and_page_validation) {
    auto origin = Origin::parse("HTTPS://Example.COM:0443/", false); BOOST_REQUIRE(origin); BOOST_CHECK_EQUAL(origin.value().value(), "https://example.com");
    BOOST_CHECK(Origin::parse("http://[::1]:8080", true));
    for (auto value : {"http://localhost", "http://127.1", "http://0177.0.0.1", "http://2130706433", "http://[0:0:0:0:0:0:0:1]", "http://user@127.0.0.1", "http://127.0.0.1/?", "http://127.0.0.1/#", "http://127.0.0.1/a", "http://127.0.0.1//", "http://127.0.0.1:0", "http://127.0.0.1:65536", "http://127.0.0.1:80x"}) BOOST_CHECK(!Origin::parse(value, true));
    BOOST_CHECK(!Origin::parse("http://127.0.0.1", false)); BOOST_CHECK(!Origin::parse("https://example.com", true));
    auto query = pageQuery({1, "a+b%/=?"}); BOOST_REQUIRE(query); BOOST_CHECK_EQUAL(query.value(), "?limit=1&cursor=a%2Bb%25%2F%3D%3F");
    BOOST_CHECK(!pageQuery({0,{}})); BOOST_CHECK(!pageQuery({201,{}})); BOOST_CHECK(!pageQuery({1,std::string(2049,'a')}));
}
BOOST_AUTO_TEST_CASE(typed_success_and_exact_integer_etag) {
    auto r = response(200, userJson()); r.headers["etag"] = "\"user-" + uid + "-9223372036854775807\"";
    auto decoded = decodeResponse(Operation::Me, r); BOOST_REQUIRE(decoded);
    const auto& user = std::get<User>(decoded.value().data); BOOST_CHECK_EQUAL(user.revision.value(), INT64_MAX); BOOST_CHECK_EQUAL(user.displayName.value(), "家庭管理员");
    r.headers["etag"] = "\"wrong\""; BOOST_CHECK(!decodeResponse(Operation::Me, r));
    auto n = nodeJson(); n["kind"] = "file"; n["size"] = "1099511627776"; n["sha256"] = std::string(64,'a');
    r = response(200, n); r.headers["etag"] = n["etag"].get<std::string>();
    decoded = decodeResponse(Operation::Stat, r); BOOST_REQUIRE(decoded); BOOST_CHECK_EQUAL(std::get<Node>(decoded.value().data).file->size.value(), 1099511627776LL);
    n["size"] = "9223372036854775808"; r = response(200,n); r.headers["etag"] = n["etag"].get<std::string>(); BOOST_CHECK(!decodeResponse(Operation::Stat,r));
}
BOOST_AUTO_TEST_CASE(all_payloads_and_pages) {
    BOOST_CHECK(decodeResponse(Operation::Register, response(201,userJson())));
    BOOST_CHECK(decodeResponse(Operation::Login,loginResponse()));
    BOOST_CHECK(decodeResponse(Operation::CreateInvite,response(201,{{"invite",inviteJson()},{"token","anof_invite_"+token}})));
    BOOST_CHECK(decodeResponse(Operation::ListInvites,response(200,{{"items",Json::array({inviteJson()})},{"next_cursor","cursor"}})));
    BOOST_CHECK(decodeResponse(Operation::Children,response(200,{{"items",Json::array({nodeJson()})},{"next_cursor",nullptr}}),1));
    BOOST_CHECK(!decodeResponse(Operation::Children,response(200,{{"items",Json::array({nodeJson(),nodeJson()})},{"next_cursor",nullptr}}),1));
    BOOST_CHECK(!decodeResponse(Operation::Children,response(200,{{"items",Json::array()},{"next_cursor","cursor"}})));
    const Json amount{{"bytes","9223372036854775807"},{"nodes","1000"}};
    const Json quota{{"limit",amount},{"used",amount},{"reserved",amount},{"revision","1"}};
    const Json privateSpace{{"id",uid},{"kind","private"},{"owner_id",uid},{"root_node_id",nid},{"quota",quota},{"my_contribution_quota",nullptr}};
    const Json publicSpace{{"id",sid},{"kind","public"},{"owner_id",nullptr},{"root_node_id",rid},{"quota",quota},{"my_contribution_quota",quota}};
    BOOST_CHECK(decodeResponse(Operation::Spaces,response(200,{{"items",Json::array({privateSpace,publicSpace})}})));
    auto n = nodeJson(); n.erase("etag"); auto r = response(201,n); r.headers["etag"] = "\"node-"+nid+"-1\""; r.headers["location"] = "/api/v1/nodes/"+nid;
    BOOST_CHECK(decodeResponse(Operation::Mkdir,r)); r.headers.erase("location"); BOOST_CHECK(!decodeResponse(Operation::Mkdir,r));
    BOOST_CHECK(decodeResponse(Operation::Logout,rawResponse(204,""))); BOOST_CHECK(decodeResponse(Operation::RevokeInvite,rawResponse(204,"")));
    BOOST_CHECK(!decodeResponse(Operation::Logout,rawResponse(204,"{}")));
}
BOOST_AUTO_TEST_CASE(strict_json_and_safe_error_classification) {
    for (const auto& body : std::vector<std::string>{"", "<html>secret</html>", "{}{}", "{\"a\":1,\"a\":2}", "{\"a\":{\"x\":1,\"\\u0078\":2}}", std::string(33,'[')+"0"+std::string(33,']'), "{\"bad\":\"\\ud800\"}"}) {
        auto r = rawResponse(200,body); auto read = decodeResponse(Operation::Me,r); BOOST_REQUIRE(!read); BOOST_CHECK(read.error().kind == ErrorKind::Protocol); BOOST_CHECK(read.error().remote == RemoteOutcome::Unknown);
        r.status = 201; auto write = decodeResponse(Operation::Login,r); BOOST_REQUIRE(!write); BOOST_CHECK(write.error().kind == ErrorKind::OutcomeUnknown);
    }
    auto j = userJson(); j["unknown"] = "ignored"; BOOST_CHECK(decodeResponse(Operation::Register,response(201,j)));
    for (const auto& value : {"2025-02-29T00:00:00.000000Z", "2026-10-11T24:00:00.000000Z", "2026-10-11T10:00:00Z"}) { j["created_at"] = value; BOOST_CHECK(!decodeResponse(Operation::Register,response(201,j))); }
    auto error = decodeResponse(Operation::Login,rejection(409,"SESSION_LIMIT_REACHED")); BOOST_REQUIRE(!error); BOOST_CHECK_EQUAL(error.error().exitCode(),3); BOOST_CHECK(error.error().remote == RemoteOutcome::Rejected);
    error = decodeResponse(Operation::Login,rejection(429,"SESSION_LIMIT_REACHED")); BOOST_REQUIRE(!error); BOOST_CHECK_EQUAL(error.error().exitCode(),6);
    error = decodeResponse(Operation::Login,rejection(500,"COMMIT_OUTCOME_UNKNOWN")); BOOST_REQUIRE(!error); BOOST_CHECK_EQUAL(error.error().exitCode(),6);
    auto r = rejection(401,"UNAUTHENTICATED"); r.headers["x-request-id"] = uid; error = decodeResponse(Operation::Me,r); BOOST_REQUIRE(!error); BOOST_CHECK_EQUAL(error.error().exitCode(),4);
    r = loginResponse(); r.headers["content-encoding"] = "gzip"; BOOST_CHECK(!decodeResponse(Operation::Login,r));
}
BOOST_AUTO_TEST_CASE(api_login_logout_persistence_and_anonymous_headers) {
    Temp temp; ProfileStore store(temp.config()); auto origin = Origin::parse("http://127.0.0.1:8080",true).value(); BOOST_REQUIRE(store.add("alice",origin));
    auto locked = store.lock("alice"); BOOST_REQUIRE(locked); MockTransport transport; ApiClient api(transport,locked.value());
    BOOST_CHECK(!api.me()); BOOST_CHECK_EQUAL(transport.calls,0);
    transport.reply = [&](const Origin& o,const HttpRequest& request) -> ClientResult<HttpResponse> {
        BOOST_CHECK_EQUAL(o.value(),origin.value()); BOOST_CHECK(!request.bearer); BOOST_CHECK(request.method == Method::Post); BOOST_CHECK_EQUAL(request.path,"/api/v1/auth/sessions");
        auto body = Json::parse(request.body.view()); BOOST_CHECK_EQUAL(body["credential_mode"].get<std::string>(),"bearer"); return loginResponse();
    };
    auto logged = api.login(LoginName::parse("Admin").value(),PasswordInput::parse("test-password-123").value()); BOOST_REQUIRE(logged); BOOST_CHECK(locked.value().profile().credential); BOOST_CHECK_EQUAL(transport.calls,1);
    BOOST_CHECK(!api.login(LoginName::parse("admin").value(),PasswordInput::parse("test-password-123").value())); BOOST_CHECK_EQUAL(transport.calls,1);
    transport.reply = [&](const Origin&,const HttpRequest& request) -> ClientResult<HttpResponse> { BOOST_REQUIRE(request.bearer); BOOST_CHECK_EQUAL(request.bearer->view(),token); return rejection(401,"UNAUTHENTICATED"); };
    BOOST_CHECK(!api.me()); BOOST_CHECK(locked.value().profile().credential);
    auto out = api.logout(); BOOST_REQUIRE(!out); BOOST_CHECK(out.error().local == LocalState::CredentialCleared); BOOST_CHECK(!locked.value().profile().credential);
    auto calls = transport.calls; BOOST_CHECK(api.logout()); BOOST_CHECK_EQUAL(transport.calls,calls);
}
BOOST_AUTO_TEST_CASE(api_save_failures_preserve_remote_outcomes_and_never_retry) {
    for (auto point : {SavePoint::BeforePublish, SavePoint::AfterPublish}) {
        Temp temp; auto origin = Origin::parse("http://127.0.0.1",true).value(); ProfileStore good(temp.config()); BOOST_REQUIRE(good.add("alice",origin));
        bool inject = true; ProfileStore store(temp.config(),[&](SavePoint p){ return !inject || p != point; });
        {
            auto held = store.lock("alice"); BOOST_REQUIRE(held); MockTransport transport; transport.reply = [](const Origin&,const HttpRequest&) -> ClientResult<HttpResponse> { return loginResponse(); };
            ApiClient api(transport,held.value()); auto result = api.login(LoginName::parse("admin").value(),PasswordInput::parse("test-password-123").value());
            BOOST_REQUIRE(!result); BOOST_CHECK(result.error().kind == ErrorKind::LocalState); BOOST_CHECK(result.error().remote == RemoteOutcome::ConfirmedSuccess);
            BOOST_CHECK(result.error().local == (point == SavePoint::BeforePublish ? LocalState::Unchanged : LocalState::Unknown)); BOOST_CHECK_EQUAL(transport.calls,1);
        }
        auto held = store.lock("alice"); BOOST_REQUIRE(held); inject = false; BOOST_REQUIRE(held.value().saveCredential(credential())); inject = true;
        MockTransport transport; transport.reply = [](const Origin&,const HttpRequest&) -> ClientResult<HttpResponse> { return failure(ErrorKind::OutcomeUnknown); };
        ApiClient api(transport,held.value()); auto result = api.logout(); BOOST_REQUIRE(!result); BOOST_CHECK(result.error().kind == ErrorKind::LocalState); BOOST_CHECK(result.error().remote == RemoteOutcome::Unknown); BOOST_CHECK_EQUAL(transport.calls,1);
    }
}
BOOST_AUTO_TEST_CASE(typed_api_paths_methods_and_body_validation) {
    Temp temp; ProfileStore store(temp.config()); BOOST_REQUIRE(store.add("alice",Origin::parse("http://127.0.0.1",true).value()));
    auto held = store.lock("alice"); BOOST_REQUIRE(held); BOOST_REQUIRE(held.value().saveCredential(credential()));
    MockTransport transport; ApiClient api(transport,held.value());
    transport.reply = [&](const Origin&,const HttpRequest& request) -> ClientResult<HttpResponse> {
        BOOST_CHECK(!request.bearer); BOOST_CHECK_EQUAL(request.path,"/api/v1/auth/register");
        const auto body = Json::parse(request.body.view()); BOOST_CHECK_EQUAL(body["password"].get<std::string>(),"password-\"\\-123"); BOOST_CHECK_EQUAL(body["invite_token"].get<std::string>(),"anof_invite_"+token);
        return response(201,userJson());
    };
    BOOST_REQUIRE(api.registerUser(LoginName::parse("admin").value(),DisplayName::parse("家庭管理员").value(),PasswordInput::parse("password-\"\\-123").value(),InviteTokenText::parse("anof_invite_"+token).value()));
    BOOST_CHECK(held.value().profile().credential);
    transport.reply = [&](const Origin&,const HttpRequest& request) -> ClientResult<HttpResponse> {
        BOOST_REQUIRE(request.bearer); BOOST_CHECK(request.method == Method::Post); BOOST_CHECK_EQUAL(request.path,"/api/v1/admin/invites");
        const auto body = Json::parse(request.body.view()); BOOST_CHECK_EQUAL(body["max_uses"].get<unsigned>(),2); BOOST_CHECK_EQUAL(body["valid_for_seconds"].get<unsigned>(),60);
        return response(201,{{"invite",inviteJson()},{"token","anof_invite_"+token}});
    };
    BOOST_REQUIRE(api.createInvite(2,60)); auto calls=transport.calls; BOOST_CHECK(!api.createInvite(0,60)); BOOST_CHECK(!api.createInvite(1,2592001)); BOOST_CHECK_EQUAL(transport.calls,calls);
    transport.reply = [&](const Origin&,const HttpRequest& request) -> ClientResult<HttpResponse> {
        BOOST_CHECK(request.method == Method::Get); BOOST_CHECK(request.body.view().empty()); BOOST_CHECK_EQUAL(request.path,"/api/v1/admin/invites?limit=1&cursor=a%2Bb");
        return response(200,{{"items",Json::array({inviteJson()})},{"next_cursor",nullptr}});
    };
    BOOST_REQUIRE(api.listInvites({1,"a+b"}));
    transport.reply = [&](const Origin&,const HttpRequest& request) -> ClientResult<HttpResponse> {
        BOOST_CHECK(request.method == Method::Delete); BOOST_CHECK_EQUAL(request.path,"/api/v1/admin/invites/"+rid); return rawResponse(204,"");
    };
    BOOST_REQUIRE(api.revokeInvite(InviteId::parse(rid).value()));
    transport.reply = [&](const Origin&,const HttpRequest& request) -> ClientResult<HttpResponse> {
        BOOST_CHECK_EQUAL(request.path,"/api/v1/nodes/"+nid+"/children?limit=100"); return response(200,{{"items",Json::array({nodeJson()})},{"next_cursor",nullptr}});
    };
    BOOST_REQUIRE(api.children(NodeId::parse(nid).value()));
    transport.reply = [&](const Origin&,const HttpRequest& request) -> ClientResult<HttpResponse> {
        BOOST_CHECK_EQUAL(request.path,"/api/v1/nodes/"+nid); auto r=response(200,nodeJson()); r.headers["etag"]="\"node-"+nid+"-1\""; return r;
    };
    BOOST_REQUIRE(api.stat(NodeId::parse(nid).value()));
    transport.reply = [&](const Origin&,const HttpRequest& request) -> ClientResult<HttpResponse> {
        BOOST_CHECK(request.method == Method::Post); BOOST_CHECK_EQUAL(request.path,"/api/v1/nodes"); const auto body=Json::parse(request.body.view()); BOOST_CHECK_EQUAL(body["name"].get<std::string>(),"照片");
        auto n=nodeJson(); n.erase("etag"); auto r=response(201,n); r.headers["etag"]="\"node-"+nid+"-1\""; r.headers["location"]="/api/v1/nodes/"+nid; return r;
    };
    BOOST_REQUIRE(api.mkdir(SpaceId::parse(sid).value(),NodeId::parse(nid).value(),"照片")); calls=transport.calls; BOOST_CHECK(!api.mkdir(SpaceId::parse(sid).value(),NodeId::parse(nid).value(),"a/b")); BOOST_CHECK_EQUAL(transport.calls,calls);
}
BOOST_AUTO_TEST_CASE(unknown_response_does_not_save_credentials) {
    Temp temp; ProfileStore store(temp.config()); BOOST_REQUIRE(store.add("alice",Origin::parse("http://127.0.0.1",true).value()));
    auto held=store.lock("alice"); BOOST_REQUIRE(held); MockTransport transport;
    transport.reply=[](const Origin&,const HttpRequest&) -> ClientResult<HttpResponse> { return rawResponse(201,"<html>bad secret response</html>"); };
    ApiClient api(transport,held.value()); auto result=api.login(LoginName::parse("admin").value(),PasswordInput::parse("test-password-123").value());
    BOOST_REQUIRE(!result); BOOST_CHECK_EQUAL(result.error().exitCode(),6); BOOST_CHECK(!held.value().profile().credential); BOOST_CHECK_EQUAL(transport.calls,1);
}

BOOST_AUTO_TEST_CASE(account_tags_payloads_and_operation_specific_errors) {
    const auto tag="\"user-"+uid+"-9223372036854775807\"";
    BOOST_REQUIRE(AccountTag::parse(tag));
    for (const auto& bad : std::vector<std::string>{"*","W/"+tag,tag+", "+tag," "+tag,"\"user-"+uid+"-0\"","\"user-"+uid+"-01\"","\"user-"+uid+"-9223372036854775808\"",tag+"\r\nInjected: x"}) BOOST_CHECK(!AccountTag::parse(bad));
    auto r=response(200,userJson());r.headers["etag"]=tag;BOOST_REQUIRE(decodeResponse(Operation::Rename,r));
    r=response(200,{{"user",userJson()},{"private_space_id",sid}});r.headers["etag"]=tag;
    BOOST_REQUIRE(decodeResponse(Operation::GetUser,r));BOOST_REQUIRE(decodeResponse(Operation::SetUserEnabled,r));
    r.headers["etag"]="\"user-"+uid+"-1\"";BOOST_CHECK(!decodeResponse(Operation::GetUser,r));
    BOOST_CHECK(decodeResponse(Operation::ListUsers,response(200,{{"items",Json::array({userJson()})},{"next_cursor",nullptr}}),1));
    BOOST_CHECK(!decodeResponse(Operation::ListUsers,response(200,{{"items",Json::array({userJson(),userJson()})},{"next_cursor",nullptr}}),2));
    const Json amount{{"bytes","1099511627776"},{"nodes","1"}};
    const Json quota{{"limit",amount},{"used",amount},{"reserved",amount},{"revision","7"}};
    for (bool contribution : {false,true}) {
        const auto qtag=contribution?"\"quota-contribution-"+sid+"-"+uid+"-7\"":"\"quota-space-"+sid+"-7\"";
        BOOST_REQUIRE(AccountTag::parse(qtag));
        r=response(200,{{"space_id",sid},{"user_id",contribution?Json(uid):Json(nullptr)},{"quota",quota}});r.headers["etag"]=qtag;
        BOOST_REQUIRE(decodeResponse(Operation::GetQuota,r));BOOST_REQUIRE(decodeResponse(Operation::SetQuotaLimit,r));
        r.headers["etag"]=tag;BOOST_CHECK(!decodeResponse(Operation::SetQuotaLimit,r));
    }
    r=rawResponse(204,"");r.headers.erase("content-type");BOOST_REQUIRE(decodeResponse(Operation::ChangePassword,r));
    r.headers["etag"]=tag;BOOST_CHECK(!decodeResponse(Operation::ChangePassword,r));
    for (auto op : {Operation::Rename,Operation::ChangePassword,Operation::SetUserEnabled,Operation::SetQuotaLimit}) {
        for (const auto& e : {std::pair{412,"PRECONDITION_FAILED"}, {428,"PRECONDITION_REQUIRED"}, {429,"RATE_LIMITED"},{403,"FORBIDDEN"}}) {
            auto rejected=decodeResponse(op,rejection(e.first,e.second));BOOST_REQUIRE(!rejected);BOOST_CHECK(rejected.error().kind==ErrorKind::Application);
        }
    }
    auto q=decodeResponse(Operation::SetQuotaLimit,rejection(409,"QUOTA_EXCEEDED"));BOOST_REQUIRE(!q);BOOST_CHECK(q.error().kind==ErrorKind::Application);
    auto invalid=decodeResponse(Operation::SetQuotaLimit,rejection(403,"QUOTA_EXCEEDED"));BOOST_REQUIRE(!invalid);BOOST_CHECK(invalid.error().kind==ErrorKind::OutcomeUnknown);
}
BOOST_AUTO_TEST_CASE(password_clear_only_after_valid_success_and_never_retries) {
    Temp temp;ProfileStore store(temp.config());BOOST_REQUIRE(store.add("admin",Origin::parse("http://127.0.0.1:1",true).value()));
    auto held=store.lock("admin");BOOST_REQUIRE(held);BOOST_REQUIRE(held.value().saveCredential(credential()));
    MockTransport transport;ApiClient api(transport,held.value());
    const auto tag=AccountTag::parse("\"user-"+uid+"-1\"").value();
    const auto password=PasswordInput::parse("fixture password").value();
    for (int status : {403,412,429,500,204}) {
        transport.reply=[&](const auto&,const HttpRequest& request)->ClientResult<HttpResponse> {
            BOOST_CHECK(request.method==Method::Post);BOOST_CHECK_EQUAL(request.path,"/api/v1/users/me/password");BOOST_REQUIRE(request.ifMatch);BOOST_CHECK_EQUAL(request.ifMatch->value(),tag.value());
            if(status==204) return rawResponse(204,"malformed");
            return rejection(status,status==403?"FORBIDDEN":status==412?"PRECONDITION_FAILED":status==429?"RATE_LIMITED":"COMMIT_OUTCOME_UNKNOWN");
        };
        const auto count=transport.calls;auto result=api.changePassword(password,password,tag);BOOST_CHECK(!result);BOOST_CHECK_EQUAL(transport.calls,count+1);BOOST_CHECK(held.value().profile().credential.has_value());
    }
    transport.reply=[](const auto&,const auto&)->ClientResult<HttpResponse>{auto r=rawResponse(204,"");r.headers.erase("content-type");return r;};
    BOOST_REQUIRE(api.changePassword(password,password,tag));BOOST_CHECK(!held.value().profile().credential);
}

BOOST_AUTO_TEST_CASE(backwards_account_timestamp_is_unknown_write_not_database_commit_evidence) {
    auto data=userJson();
    data["updated_at"]="2026-10-11T09:59:59.999999Z";
    auto wire=response(200,data);
    wire.headers["etag"]="\"user-"+uid+"-9223372036854775807\"";
    auto changed=decodeResponse(Operation::Rename,wire);
    BOOST_REQUIRE(!changed);
    BOOST_CHECK(changed.error().kind==ErrorKind::OutcomeUnknown);
    BOOST_CHECK_EQUAL(changed.error().exitCode(),6);
    auto read=decodeResponse(Operation::Me,wire);
    BOOST_REQUIRE(!read);
    BOOST_CHECK(read.error().kind==ErrorKind::Protocol);
    data["updated_at"]=data["created_at"];
    wire=response(200,data);wire.headers["etag"]="\"user-"+uid+"-9223372036854775807\"";
    BOOST_CHECK(decodeResponse(Operation::Rename,wire));
}
