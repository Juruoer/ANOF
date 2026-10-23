#define BOOST_TEST_MODULE ClientProcess
#include <tests/integration/ClientProcess.h>
#include <anof/runtime/InstancePasswordPolicy.h>
#include <anof/application/maintenance/Initialization.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
using namespace anof::testing::cli;
namespace {
constexpr const char* password="fixture secret password";
void add(const Temp& temp,const std::string& name,unsigned port) { run(temp,"",{"profile","add",name,"--server","http://127.0.0.1:"+std::to_string(port),"--development-http"}); }
void initialize(const std::filesystem::path& root) {
    Process p({"init","--root",root.string(),"--admin-login","admin","--admin-display-name","Administrator","--development-http",
        "--content-budget-bytes","1048576","--disk-safety-min-bytes","12345","--disk-safety-percent","17","--private-bytes","1099511627776","--private-nodes","4","--public-bytes","1099511627776","--public-nodes","10","--contribution-bytes","1099511627776","--contribution-nodes","10"},true);
    p.input("Administrator password: ",password);p.input("Confirm password: ",password);p.finish(0); BOOST_CHECK(p.out.find("ANOF_INITIALIZED")!=std::string::npos); BOOST_CHECK(p.tty.find(password)==std::string::npos);
    BOOST_CHECK_EQUAL(jsonFile(root/".anof/instance.json")["format_version"].get<int>(), anof::InitializationIdentity::formatVersion);
    {
        namespace infra = anof::infrastructure;
        auto locks = infra::system::makeLinuxInstanceLockProvider();
        auto loader = infra::config::makeTomlInstanceConfigurationLoader();
        BOOST_REQUIRE(locks && loader);
        auto lease = locks.value()->acquire(root); BOOST_REQUIRE(lease);
        auto config = loader.value()->load(*lease.value()); BOOST_REQUIRE(config);
        BOOST_CHECK_EQUAL(config.value().diskSafety.minimum.value(), 12345);
        BOOST_CHECK_EQUAL(config.value().diskSafety.percent.value(), 17);
        auto store = infra::sqlite::openSqliteTransferMetadataStore(root/".anof/metadata.sqlite3",
            {anof::runtime::instancePasswordPolicy().verification, {}});
        BOOST_REQUIRE(store);
        auto read = store.value()->beginTransferRead(); BOOST_REQUIRE(read);
        auto storage = read.value()->storage().get(); BOOST_REQUIRE(storage);
        BOOST_CHECK_EQUAL(storage.value().budget.value(), 1048576);
        BOOST_CHECK_EQUAL(storage.value().reserved.value(), 0);
        BOOST_CHECK_EQUAL(storage.value().retained.value(), 0);
    }
    file(root/".anof/config.toml","development_http=true\nport=0\n");
}
Json privateSpace(const Json& result) {for(const auto& s:result.at("data").at("items")) if(s.at("kind")=="private") return s; throw std::runtime_error("private space missing");}
}
BOOST_AUTO_TEST_CASE(real_init_invitation_two_users_pagination_privacy_quota_logout_restart) {
    Temp temp; const auto root=temp.path/"instance"; std::filesystem::create_directory(root); chmod(root.c_str(),0700); initialize(root);
    Process server({"serve","--root",root.string()}); const auto port=server.port();
    file(root/".anof/config.toml","development_http=true\nport="+std::to_string(port)+"\n");
    for(auto name:{"admin","alice","bob"}) add(temp,name,port);
    login(temp,"admin","admin",password);
    const auto invitation=run(temp,"admin",{"invite","create","--max-uses","2"}); const auto token=invitation["data"]["token"].get<std::string>();
    for(auto name:{"alice","bob"}) {
        Process registration(command(temp,name,{"register","--login",name,"--display-name",name}),true);
        registration.input("Invitation token: ",token);registration.input("New password: ",password);registration.input("Confirm password: ",password);
        registration.success();BOOST_CHECK(registration.tty.find(token)==std::string::npos);BOOST_CHECK(registration.tty.find(password)==std::string::npos);
        BOOST_CHECK(jsonFile(temp.path/"config/profiles"/(std::string(name)+".json"))["credential"].is_null());
        login(temp,name,name,password);
    }
    const auto alice=privateSpace(run(temp,"alice",{"spaces"})),bob=privateSpace(run(temp,"bob",{"spaces"}));
    BOOST_CHECK(alice["id"]!=bob["id"]);BOOST_CHECK_EQUAL(alice["quota"]["limit"]["bytes"].get<std::string>(),"1099511627776");
    const auto aliceRoot=alice["root_node_id"].get<std::string>(),bobRoot=bob["root_node_id"].get<std::string>(),space=alice["id"].get<std::string>();
    BOOST_CHECK_EQUAL(run(temp,"alice",{"me"})["data"]["login"].get<std::string>(),"alice"); BOOST_CHECK_EQUAL(run(temp,"bob",{"me"})["data"]["login"].get<std::string>(),"bob");
    Process hidden(command(temp,"alice",{"stat",bobRoot}));BOOST_CHECK_EQUAL(hidden.failure(3)["code"].get<std::string>(),"NOT_FOUND");
    Process hiddenCreate(command(temp,"alice",{"mkdir","--space",bob["id"].get<std::string>(),"--parent",bobRoot,"--name","denied"})); BOOST_CHECK_EQUAL(hiddenCreate.failure(3)["code"].get<std::string>(),"NOT_FOUND");
    std::string node;
    for(auto name:{"alpha","beta","gamma"}) {auto made=run(temp,"alice",{"mkdir","--space",space,"--parent",aliceRoot,"--name",name});node=made["data"]["id"]; BOOST_CHECK(!made["data"].contains("etag")); BOOST_CHECK(made["meta"]["etag"].is_string());}
    Process over(command(temp,"alice",{"mkdir","--space",space,"--parent",aliceRoot,"--name","delta"}));BOOST_CHECK_EQUAL(over.failure(3)["code"].get<std::string>(),"QUOTA_EXCEEDED");
    auto page=run(temp,"alice",{"ls",aliceRoot,"--limit","2"});BOOST_CHECK_EQUAL(page["data"]["items"].size(),2); const auto cursor=page["data"]["next_cursor"].get<std::string>();
    auto last=run(temp,"alice",{"ls",aliceRoot,"--limit","2","--cursor",cursor});BOOST_CHECK_EQUAL(last["data"]["items"].size(),1);BOOST_CHECK(last["data"]["next_cursor"].is_null());
    auto stat=run(temp,"alice",{"stat",node});BOOST_CHECK_EQUAL(stat["data"]["etag"].get<std::string>(),stat["meta"]["etag"].get<std::string>());
    auto shown=run(temp,"",{"profile","show","alice"}); const auto saved=jsonFile(temp.path/"config/profiles/alice.json"); const auto access=saved["credential"]["access_token"].get<std::string>();
    BOOST_CHECK(shown.dump().find(access)==std::string::npos); BOOST_CHECK(run(temp,"",{"profile","list"}).dump().find(access)==std::string::npos);
    struct stat permissions{};BOOST_REQUIRE_EQUAL(::stat((temp.path/"config/profiles/alice.json").c_str(),&permissions),0);BOOST_CHECK_EQUAL(permissions.st_mode&0777,0600);
    auto invites=run(temp,"admin",{"invite","list"});BOOST_CHECK_EQUAL(invites["data"]["items"].size(),1);BOOST_CHECK_EQUAL(invites["data"]["items"][0]["used_count"].get<int>(),2);
    const auto revoke=run(temp,"admin",{"invite","create"});run(temp,"admin",{"invite","revoke",revoke["data"]["invite"]["id"].get<std::string>()});
    auto oldBob=jsonFile(temp.path/"config/profiles/bob.json");run(temp,"bob",{"logout"});BOOST_CHECK(jsonFile(temp.path/"config/profiles/bob.json")["credential"].is_null());
    file(temp.path/"config/profiles/bob.json",oldBob.dump()); Process stale(command(temp,"bob",{"me"}));BOOST_CHECK_EQUAL(stale.failure(3)["code"].get<std::string>(),"UNAUTHENTICATED");BOOST_CHECK(!jsonFile(temp.path/"config/profiles/bob.json")["credential"].is_null());
    Process clearStale(command(temp,"bob",{"logout"}));BOOST_CHECK_EQUAL(clearStale.failure(3)["local_state"].get<std::string>(),"credential_cleared");
    server.stop();BOOST_CHECK(server.out.find(access)==std::string::npos);BOOST_CHECK(server.out.find(password)==std::string::npos);
    Process restarted({"serve","--root",root.string()}); BOOST_CHECK_EQUAL(restarted.port(),port);
    BOOST_CHECK_EQUAL(run(temp,"alice",{"stat",node})["data"]["id"].get<std::string>(),node);
    BOOST_CHECK_EQUAL(run(temp,"alice",{"me"})["data"]["login"].get<std::string>(),"alice");
    Process reused(command(temp,"bob",{"register","--login","charlie","--display-name","Charlie"}),true);reused.input("Invitation token: ",token);reused.input("New password: ",password);reused.input("Confirm password: ",password);BOOST_CHECK_EQUAL(reused.failure(3)["code"].get<std::string>(),"INVITE_UNAVAILABLE");
    Process closed(command(temp,"admin",{"invite","create"}),false,true);closed.finish(5);auto failure=Json::parse(closed.err)["error"];BOOST_CHECK_EQUAL(failure["remote_outcome"].get<std::string>(),"confirmed_success");BOOST_CHECK(closed.err.find("anof_invite_")==std::string::npos);
    Process human(command(temp,"alice",{"ls",aliceRoot},false));human.finish(0);BOOST_CHECK(human.out.find("next_cursor: (end)")!=std::string::npos);
    run(temp,"alice",{"logout"});Process absent(command(temp,"alice",{"me"}));BOOST_CHECK_EQUAL(absent.failure(3)["code"].get<std::string>(),"AUTHENTICATION_REQUIRED");
    restarted.stop();Process lost(command(temp,"admin",{"logout"}));auto lostError=lost.failure(6);BOOST_CHECK_EQUAL(lostError["local_state"].get<std::string>(),"credential_cleared");run(temp,"admin",{"logout"});
}
BOOST_AUTO_TEST_CASE(help_arguments_terminal_errors_and_lock_before_prompt) {
    Temp temp;
    Process help(command(temp,"",{"login","--help"}));help.finish(0);BOOST_CHECK(!std::filesystem::exists(temp.path/"config"));BOOST_CHECK(help.out.find("Client exit codes")!=std::string::npos);
    Process closedHelp(command(temp,"",{"login","--help"}),false,true);closedHelp.failure(5);
    for(const auto& args:std::vector<std::vector<std::string>>{{"me"},{"--profile","bad.name","me"},{"--profile","alice","ls","invalid"},{"--profile","alice","invite","list","--limit","01"},{"profile","add","alice","--server","http://localhost","--development-http"},{"serve","--root","/nonexistent"},{"--profile","alice","login","--login","alice","--password","DO_NOT_PRINT"}}) {
        Process p(command(temp,"",args));p.failure(2);BOOST_CHECK(p.err.find("DO_NOT_PRINT")==std::string::npos);
    }
    BOOST_CHECK(!std::filesystem::exists(temp.path/"config")); add(temp,"alice",1);
    Process noTty(command(temp,"alice",{"login","--login","alice"}));BOOST_CHECK_EQUAL(noTty.failure(5)["code"].get<std::string>(),"TERMINAL_REQUIRED");
    {
        Process locked(command(temp,"alice",{"login","--login","alice"}),true);locked.until([&]{return locked.tty.find("Password: ")!=std::string::npos;});
        Process rival(command(temp,"alice",{"login","--login","alice"}),true);BOOST_CHECK_EQUAL(rival.failure(5)["code"].get<std::string>(),"PROFILE_BUSY");BOOST_CHECK(rival.tty.empty());
        BOOST_REQUIRE_EQUAL(kill(locked.pid,SIGINT),0);BOOST_CHECK_EQUAL(locked.failure(130)["remote_outcome"].get<std::string>(),"not_sent");
    }
    Process longPassword(command(temp,"alice",{"login","--login","alice"}),true);longPassword.input("Password: ",std::string(1025,'x'));longPassword.failure(2);BOOST_CHECK(longPassword.tty.find(std::string(10,'x'))==std::string::npos);
    Process badInvite(command(temp,"alice",{"register","--login","alice","--display-name","Alice"}),true);badInvite.input("Invitation token: ",std::string(56,'x'));badInvite.failure(2);BOOST_CHECK(badInvite.tty.find("New password")==std::string::npos);
    Process mismatch(command(temp,"alice",{"register","--login","alice","--display-name","Alice"}),true);mismatch.input("Invitation token: ","anof_invite_"+std::string(43,'A'));mismatch.input("New password: ",password);mismatch.input("Confirm password: ","different password");mismatch.failure(2);
    Process misplaced({"--config-dir",(temp.path/"config").string(),"--json","me","--profile","alice"});misplaced.failure(2);
    Process pipe(command(temp,"",{"profile","show","alice"}),false,true);pipe.finish(5);BOOST_CHECK_EQUAL(Json::parse(pipe.err)["error"]["remote_outcome"].get<std::string>(),"not_sent");
}

#include <arpa/inet.h>
#include <sys/socket.h>
namespace {
struct Peer {
    int listener=-1, connection=-1;
    unsigned port=0;
    Peer() {
        listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0); BOOST_REQUIRE(listener>=0);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        BOOST_REQUIRE_EQUAL(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof address),0);BOOST_REQUIRE_EQUAL(listen(listener,2),0);
        socklen_t length=sizeof address;BOOST_REQUIRE_EQUAL(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length),0);port=ntohs(address.sin_port);
    }
    ~Peer() {if(connection>=0)close(connection);if(listener>=0)close(listener);}
    void request() {
        pollfd waiting{listener,POLLIN,0};BOOST_REQUIRE(poll(&waiting,1,3000)>0);connection=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);BOOST_REQUIRE(connection>=0);
        std::string bytes;
        for(;;) {
            pollfd data{connection,POLLIN,0};BOOST_REQUIRE(poll(&data,1,3000)>0);char buffer[4096];const auto n=recv(connection,buffer,sizeof buffer,0);BOOST_REQUIRE(n>0);bytes.append(buffer,static_cast<std::size_t>(n));BOOST_REQUIRE(bytes.size()<65536);
            auto end=bytes.find("\r\n\r\n");if(end==std::string::npos)continue;
            std::size_t size=0;auto length=bytes.find("Content-Length: ");if(length!=std::string::npos)size=std::stoul(bytes.substr(length+16));
            if(bytes.size()>=end+4+size)break;
        }
    }
    void respond(int status,const Json& body) {
        const auto data=status==204?std::string{}:body.dump();
        std::string text="HTTP/1.1 "+std::to_string(status)+" Result\r\nContent-Type: application/json\r\nX-Request-ID: 40000000-0000-4000-8000-000000000001\r\nContent-Length: "+std::to_string(data.size())+"\r\nConnection: close\r\n\r\n"+data;
        std::string_view rest=text;
        while(!rest.empty()){auto n=send(connection,rest.data(),rest.size(),MSG_NOSIGNAL);BOOST_REQUIRE(n>0);rest.remove_prefix(static_cast<std::size_t>(n));}
    }
    void noRetry() {pollfd p{listener,POLLIN,0};BOOST_CHECK_EQUAL(poll(&p,1,0),0);}
};
const std::string fixtureToken(43,'A');
Json storedCredential() {return {{"access_token",fixtureToken},{"expires_at","2026-10-18T10:00:00.000000Z"},{"user_id","10000000-0000-4000-8000-000000000001"},{"login","alice"}};}
Json loginReply() {
    return {{"access_token",fixtureToken},{"token_type","Bearer"},{"expires_at","2026-10-18T10:00:00.000000Z"},
        {"user",{{"id","10000000-0000-4000-8000-000000000001"},{"login","alice"},{"display_name","Alice"},{"role","member"},{"enabled",true},{"revision","1"},{"created_at","2026-10-11T10:00:00.000000Z"},{"updated_at","2026-10-11T10:00:00.000000Z"}}}};
}
void credential(const Temp& temp) {const auto path=temp.path/"config/profiles/alice.json";auto j=jsonFile(path);j["credential"]=storedCredential();file(path,j.dump());}
}
BOOST_AUTO_TEST_CASE(network_interrupt_classification_and_no_retry) {
    for(bool write:{false,true}) {
        Temp temp;Peer peer;add(temp,"alice",peer.port);credential(temp);
        Process child(command(temp,"alice",write?std::vector<std::string>{"mkdir","--space","30000000-0000-4000-8000-000000000001","--parent","20000000-0000-4000-8000-000000000001","--name","new"}:std::vector<std::string>{"me"}));
        peer.request();BOOST_REQUIRE_EQUAL(kill(child.pid,SIGINT),0);auto e=child.failure(write?6:130);BOOST_CHECK_EQUAL(e["remote_outcome"].get<std::string>(),"unknown");BOOST_CHECK(child.err.find(fixtureToken)==std::string::npos);peer.noRetry();
        BOOST_CHECK(!jsonFile(temp.path/"config/profiles/alice.json")["credential"].is_null());
    }
}
BOOST_AUTO_TEST_CASE(remote_success_with_persistence_or_output_failure) {
    for(bool logout:{false,true}) {
        Temp temp;Peer peer;add(temp,"alice",peer.port);if(logout)credential(temp);
        Process child(command(temp,"alice",logout?std::vector<std::string>{"logout"}:std::vector<std::string>{"login","--login","alice"}),!logout);
        if(!logout)child.input("Password: ",password);
        peer.request();
        // 持锁加载之后以目录占据目标名，令真实 rename 系统调用失败；原数据保留在同一测试目录。
        const auto path=temp.path/"config/profiles/alice.json",saved=temp.path/"config/profiles/original";
        std::filesystem::rename(path,saved);std::filesystem::create_directory(path);
        peer.respond(logout?204:201,logout?Json(nullptr):loginReply());auto e=child.failure(5);BOOST_CHECK_EQUAL(e["remote_outcome"].get<std::string>(),"confirmed_success");BOOST_CHECK_EQUAL(e["local_state"].get<std::string>(),"unchanged");
        BOOST_CHECK(child.err.find(fixtureToken)==std::string::npos);BOOST_CHECK(child.err.find(password)==std::string::npos);peer.noRetry();
        std::filesystem::remove(path);std::filesystem::rename(saved,path);BOOST_CHECK_EQUAL(jsonFile(path)["credential"].is_null(),!logout);
        for(const auto& item:std::filesystem::directory_iterator(path.parent_path()))BOOST_CHECK(item.path().filename().string().find(".tmp-")!=0);
    }
    Temp temp;Peer peer;add(temp,"alice",peer.port);
    Process child(command(temp,"alice",{"login","--login","alice"}),true,true);child.input("Password: ",password);peer.request();peer.respond(201,loginReply());child.finish(5);
    auto e=Json::parse(child.err)["error"];BOOST_CHECK_EQUAL(e["remote_outcome"].get<std::string>(),"confirmed_success");BOOST_CHECK_EQUAL(e["local_state"].get<std::string>(),"credential_saved");BOOST_CHECK(child.err.find(fixtureToken)==std::string::npos);BOOST_CHECK(!jsonFile(temp.path/"config/profiles/alice.json")["credential"].is_null());peer.noRetry();
}

BOOST_AUTO_TEST_CASE(account_cli_real_rename_users_quotas_disable_password_and_relogin) {
    Temp temp;const auto root=temp.path/"instance";std::filesystem::create_directory(root);chmod(root.c_str(),0700);initialize(root);
    Process server({"serve","--root",root.string()});const auto port=server.port();
    for(auto name:{"admin","alice","alice-other"})add(temp,name,port);
    login(temp,"admin","admin",password);
    auto invite=run(temp,"admin",{"invite","create"})["data"]["token"].get<std::string>();
    Process registration(command(temp,"alice",{"register","--login","alice","--display-name","Alice"}),true);
    registration.input("Invitation token: ",invite);registration.input("New password: ",password);registration.input("Confirm password: ",password);registration.success();
    login(temp,"alice","alice",password);
    auto me=run(temp,"alice",{"me"});const auto user=me["data"]["id"].get<std::string>(),oldTag=me["meta"]["etag"].get<std::string>();
    auto renamed=run(temp,"alice",{"account","rename","--display-name","New Alice","--if-match",oldTag});
    const auto tag=renamed["meta"]["etag"].get<std::string>();BOOST_CHECK(tag!=oldTag);
    auto unchanged=run(temp,"alice",{"account","rename","--display-name","New Alice","--if-match",tag});BOOST_CHECK_EQUAL(unchanged["meta"]["etag"].get<std::string>(),tag);
    Process stale(command(temp,"alice",{"account","rename","--display-name","New Alice","--if-match",oldTag}));BOOST_CHECK_EQUAL(stale.failure(3)["code"].get<std::string>(),"PRECONDITION_FAILED");
    Process denied(command(temp,"alice",{"user","show","--id",user}));BOOST_CHECK_EQUAL(denied.failure(3)["code"].get<std::string>(),"FORBIDDEN");
    auto users=run(temp,"admin",{"user","list","--limit","1"});BOOST_CHECK_EQUAL(users["data"]["items"].size(),1);
    auto last=run(temp,"admin",{"user","list","--limit","1","--cursor",users["data"]["next_cursor"].get<std::string>()});BOOST_CHECK_EQUAL(last["data"]["items"].size(),1);BOOST_CHECK(last["data"]["next_cursor"].is_null());
    auto details=run(temp,"admin",{"user","show","--id",user});const auto space=details["data"]["private_space_id"].get<std::string>();
    auto userSpace=privateSpace(run(temp,"alice",{"spaces"}));const auto parent=userSpace["root_node_id"].get<std::string>();
    Process hidden(command(temp,"admin",{"stat",parent}));BOOST_CHECK_EQUAL(hidden.failure(3)["code"].get<std::string>(),"NOT_FOUND");
    auto quota=run(temp,"admin",{"quota","show","--space-id",space});const auto quotaTag=quota["meta"]["etag"].get<std::string>();
    Process below(command(temp,"admin",{"quota","set","--space-id",space,"--bytes","0","--nodes","0","--if-match",quotaTag}));auto belowError=below.failure(3);BOOST_CHECK_EQUAL(belowError["http_status"].get<int>(),409);
    auto set=run(temp,"admin",{"quota","set","--space-id",space,"--bytes","1099511627776","--nodes","1","--if-match",quotaTag});
    Process full(command(temp,"alice",{"mkdir","--space",space,"--parent",parent,"--name","full"}));BOOST_CHECK_EQUAL(full.failure(3)["http_status"].get<int>(),403);
    run(temp,"admin",{"quota","set","--space-id",space,"--bytes","1099511627776","--nodes","4","--if-match",set["meta"]["etag"].get<std::string>()});
    auto before=run(temp,"admin",{"quota","show","--space-id",space});run(temp,"alice",{"mkdir","--space",space,"--parent",parent,"--name","counted"});
    Process revision(command(temp,"admin",{"quota","set","--space-id",space,"--bytes","1099511627776","--nodes","5","--if-match",before["meta"]["etag"].get<std::string>()}));BOOST_CHECK_EQUAL(revision.failure(3)["code"].get<std::string>(),"PRECONDITION_FAILED");
    const auto allSpaces=run(temp,"admin",{"spaces"});
    for(const auto& s:allSpaces["data"]["items"]) if(s["kind"]=="public") {
        const auto publicId=s["id"].get<std::string>();
        for(bool contribution:{false,true}) {
            std::vector<std::string> show{"quota","show","--space-id",publicId};if(contribution){show.push_back("--user-id");show.push_back(user);}
            const auto q=run(temp,"admin",show);auto args=show;args[1]="set";
            args.insert(args.end(),{"--bytes","1099511627776","--nodes","8","--if-match",q["meta"]["etag"].get<std::string>()});run(temp,"admin",args);
        }
    }
    auto disabled=run(temp,"admin",{"user","disable","--id",user,"--if-match",tag});
    Process revoked(command(temp,"alice",{"me"}));BOOST_CHECK_EQUAL(revoked.failure(3)["code"].get<std::string>(),"UNAUTHENTICATED");
    run(temp,"admin",{"user","enable","--id",user,"--if-match",disabled["meta"]["etag"].get<std::string>()});
    Process stillRevoked(command(temp,"alice",{"me"}));BOOST_CHECK_EQUAL(stillRevoked.failure(3)["code"].get<std::string>(),"UNAUTHENTICATED");
    Process clear(command(temp,"alice",{"logout"}));clear.failure(3);
    // 重启重置本用例的限流桶，避免为功能验收等待或扩大生产限额。
    server.stop();Process restarted({"serve","--root",root.string()});const auto newPort=restarted.port();
    for(auto name:{"admin","alice","alice-other"}) {const auto path=temp.path/"config/profiles"/(std::string(name)+".json");auto p=jsonFile(path);p["server"]="http://127.0.0.1:"+std::to_string(newPort);file(path,p.dump());}
    login(temp,"alice","alice",password);login(temp,"alice-other","alice",password);
    const auto current=run(temp,"alice",{"me"})["meta"]["etag"].get<std::string>();const auto otherBefore=jsonFile(temp.path/"config/profiles/alice-other.json");
    constexpr const char* next="a different fixture password";
    Process change(command(temp,"alice",{"account","password","--if-match",current}),true);
    change.input("Current password: ",password);change.input("New password: ",next);change.input("Confirm password: ",next);auto changed=change.success();
    BOOST_CHECK_EQUAL(changed["meta"]["http_status"].get<int>(),204);BOOST_CHECK(changed["data"].is_null());
    BOOST_CHECK(change.tty.find(password)==std::string::npos);BOOST_CHECK(change.tty.find(next)==std::string::npos);
    BOOST_CHECK(jsonFile(temp.path/"config/profiles/alice.json")["credential"].is_null());BOOST_CHECK(jsonFile(temp.path/"config/profiles/alice-other.json")==otherBefore);
    Process other(command(temp,"alice-other",{"me"}));BOOST_CHECK_EQUAL(other.failure(3)["code"].get<std::string>(),"UNAUTHENTICATED");
    restarted.stop();Process finalServer({"serve","--root",root.string()});const auto finalPort=finalServer.port();
    const auto path=temp.path/"config/profiles/alice.json";auto profile=jsonFile(path);profile["server"]="http://127.0.0.1:"+std::to_string(finalPort);file(path,profile.dump());
    login(temp,"alice","alice",next);BOOST_CHECK_EQUAL(run(temp,"alice",{"me"})["data"]["display_name"].get<std::string>(),"New Alice");
    finalServer.stop();
}

BOOST_AUTO_TEST_CASE(password_cli_unknown_rejection_clear_failure_and_output_failure) {
    const auto tag="\"user-10000000-0000-4000-8000-000000000001-1\"";
    for(int scenario:{0,1,2,3}) {
        Temp temp;Peer peer;add(temp,"alice",peer.port);credential(temp);
        Process child(command(temp,"alice",{"account","password","--if-match",tag}),true,scenario==3);
        child.input("Current password: ",password);child.input("New password: ",password);child.input("Confirm password: ",password);peer.request();
        const auto path=temp.path/"config/profiles/alice.json",saved=temp.path/"config/profiles/original";
        if(scenario==2){std::filesystem::rename(path,saved);std::filesystem::create_directory(path);}
        if(scenario==0) {close(peer.connection);peer.connection=-1;}
        else if(scenario==1) peer.respond(403,{{"error",{{"code","FORBIDDEN"},{"message","rejected"},{"details",Json::object()},{"request_id","40000000-0000-4000-8000-000000000001"}}}});
        else {
            const std::string response="HTTP/1.1 204 No Content\r\nX-Request-ID: 40000000-0000-4000-8000-000000000001\r\nConnection: close\r\n\r\n";
            BOOST_REQUIRE_EQUAL(send(peer.connection,response.data(),response.size(),MSG_NOSIGNAL),static_cast<ssize_t>(response.size()));
        }
        auto error=child.failure(scenario==0?6:scenario==1?3:5);
        if(scenario==0) BOOST_CHECK(error["message"].get<std::string>().find("explicitly log in again")!=std::string::npos);
        BOOST_CHECK_EQUAL(error["remote_outcome"].get<std::string>(),scenario==0?"unknown":scenario==1?"rejected":"confirmed_success");
        BOOST_CHECK_EQUAL(error["local_state"].get<std::string>(),scenario==3?"credential_cleared":"unchanged");
        if(scenario==2){std::filesystem::remove(path);std::filesystem::rename(saved,path);}
        BOOST_CHECK_EQUAL(jsonFile(path)["credential"].is_null(),scenario==3);peer.noRetry();
        BOOST_CHECK(child.err.find(password)==std::string::npos);BOOST_CHECK(child.tty.find(password)==std::string::npos);
    }
}
BOOST_AUTO_TEST_CASE(account_cli_validates_before_prompt_and_password_confirmation) {
    Temp temp;add(temp,"alice",1);credential(temp);
    const std::string tag="\"user-10000000-0000-4000-8000-000000000001-1\"";
    for(auto args:std::vector<std::vector<std::string>>{{"account","password","--if-match","*"},{"account","password","--if-match",tag,"--password","DO_NOT_PRINT"},{"quota","set","--space-id","30000000-0000-4000-8000-000000000001","--bytes","9223372036854775808","--nodes","1","--if-match",tag}}) {
        Process p(command(temp,"alice",args),true);p.failure(2);BOOST_CHECK(p.tty.empty());BOOST_CHECK(p.err.find("DO_NOT_PRINT")==std::string::npos);
    }
    Process mismatch(command(temp,"alice",{"account","password","--if-match",tag}),true);
    mismatch.input("Current password: ",password);mismatch.input("New password: ",password);mismatch.input("Confirm password: ","different password");mismatch.failure(2);
    BOOST_CHECK(!jsonFile(temp.path/"config/profiles/alice.json")["credential"].is_null());
    Process pending(command(temp,"alice",{"account","password","--if-match",tag}),true);
    pending.until([&]{return pending.tty.find("Current password: ")!=std::string::npos;});
    Process rival(command(temp,"alice",{"account","password","--if-match",tag}),true);
    BOOST_CHECK_EQUAL(rival.failure(5)["code"].get<std::string>(),"PROFILE_BUSY");BOOST_CHECK(rival.tty.empty());
    BOOST_REQUIRE_EQUAL(kill(pending.pid,SIGINT),0);BOOST_CHECK_EQUAL(pending.failure(130)["remote_outcome"].get<std::string>(),"not_sent");
}

BOOST_AUTO_TEST_CASE(init_rejects_missing_budget_and_finish_overrides_before_prompt) {
    Temp temp;
    const auto root = temp.path / "instance";
    std::filesystem::create_directory(root); chmod(root.c_str(), 0700);
    Process missing({"init", "--root", root.string(), "--admin-login", "admin", "--admin-display-name", "Admin",
        "--development-http", "--private-bytes", "0", "--private-nodes", "1", "--public-bytes", "0",
        "--public-nodes", "1", "--contribution-bytes", "0", "--contribution-nodes", "1"}, true);
    missing.finish(2);
    BOOST_CHECK(missing.tty.empty());
    for (const auto* flag : {"--content-budget-bytes", "--disk-safety-min-bytes", "--disk-safety-percent"}) {
        Process rejected({"init", "--root", root.string(), "--finish", flag, "0"}, true);
        rejected.finish(2);
        BOOST_CHECK(rejected.tty.empty());
    }
    BOOST_CHECK(std::filesystem::is_empty(root));
}

BOOST_AUTO_TEST_CASE(process_failure_summary_excludes_secrets_and_raw_messages) {
    Process process({"--help"});process.finish(0);
    process.err=Json{{"error",{{"code","OUTCOME_UNKNOWN"},{"request_id","10000000-0000-4000-8000-000000000001"},
        {"http_status",500},{"remote_outcome","unknown"},{"local_state","unchanged"},
        {"message","private message marker"},{"access_token","private token marker"}}}}.dump();
    auto diagnostic=process.failureSummary();
    BOOST_CHECK(diagnostic.find("OUTCOME_UNKNOWN")!=std::string::npos);
    BOOST_CHECK(diagnostic.find("http_status=500")!=std::string::npos);
    BOOST_CHECK(diagnostic.find("private")==std::string::npos);
    process.err="unstructured private token marker";
    diagnostic=process.failureSummary();
    BOOST_CHECK(diagnostic.find("stderr_format=non_json")!=std::string::npos);
    BOOST_CHECK(diagnostic.find("private")==std::string::npos);
    process.err.clear();
}

BOOST_AUTO_TEST_CASE(drive_cli_confirmation_unknown_and_conflict_do_not_retry) {
    const std::string node="20000000-0000-4000-8000-000000000001", tag="\"node-"+node+"-1\"";
    for(int mode:{0,1,2}) {
        Temp temp;Peer peer;add(temp,"alice",peer.port);credential(temp);
        Process child(command(temp,"alice",{"rename",node,"--name","after","--if-match",tag}));peer.request();
        if(mode==0)peer.respond(200,{{"unexpected","untrusted"}});
        else peer.respond(mode==1?500:412,{{"error",{{"code",mode==1?"COMMIT_OUTCOME_UNKNOWN":"PRECONDITION_FAILED"},{"message","never print this remote message"},{"request_id","40000000-0000-4000-8000-000000000001"},{"details",Json::object()}}}});
        auto e=child.failure(mode==2?3:6);BOOST_CHECK_EQUAL(e["operation"].get<std::string>(),"rename");BOOST_CHECK_EQUAL(e["source_id"].get<std::string>(),node);BOOST_CHECK_EQUAL(e["remote_outcome"].get<std::string>(),mode==2?"rejected":"unknown");
        BOOST_CHECK(child.err.find(fixtureToken)==std::string::npos);BOOST_CHECK(child.err.find("never print this remote message")==std::string::npos);peer.noRetry();
    }
    Temp temp;Peer peer;add(temp,"alice",peer.port);credential(temp);
    Process missing(command(temp,"alice",{"trash","purge",node,"--if-match","\"trash-"+node+"-1\""}));BOOST_CHECK_EQUAL(missing.failure(2)["operation"].get<std::string>(),"trash purge");peer.noRetry();
    Process interrupted(command(temp,"alice",{"rm",node,"--if-match",tag}));peer.request();BOOST_REQUIRE_EQUAL(kill(interrupted.pid,SIGINT),0);auto e=interrupted.failure(6);BOOST_CHECK_EQUAL(e["source_id"].get<std::string>(),node);peer.noRetry();
}
