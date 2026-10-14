#define BOOST_TEST_MODULE AnofClientProfiles
#include <boost/test/unit_test.hpp>
#include <anof/client/tests/Fixtures.h>
#include <fstream>
#include <fcntl.h>
#include <sys/file.h>
using namespace anof;
using namespace anof::client;
using namespace anof::client::test;
namespace {
Origin origin() { return Origin::parse("http://127.0.0.1:8080",true).value(); }
void replace(const std::filesystem::path& path, const std::string& bytes) { std::ofstream file(path,std::ios::binary|std::ios::trunc); file << bytes; }
std::string read(const std::filesystem::path& path) { std::ifstream file(path); return {std::istreambuf_iterator<char>(file),{}}; }
mode_t mode(const std::filesystem::path& path) { struct stat st{}; if (stat(path.c_str(),&st)) throw std::runtime_error("stat failed"); return st.st_mode & 07777; }
struct Environment {
    std::string key; std::optional<std::string> old;
    Environment(std::string keyValue,const char* value) : key(std::move(keyValue)) { if (auto p = getenv(key.c_str())) old = p; if (value) setenv(key.c_str(),value,1); else unsetenv(key.c_str()); }
    ~Environment() { if (old) setenv(key.c_str(),old->c_str(),1); else unsetenv(key.c_str()); }
};
}
BOOST_AUTO_TEST_CASE(paths_names_and_missing_directory) {
    Temp temp; ProfileStore store(temp.config()); auto listed = store.list(); BOOST_REQUIRE(listed); BOOST_CHECK(listed.value().empty()); BOOST_CHECK(!std::filesystem::exists(temp.config()));
    BOOST_CHECK(!store.show("alice")); BOOST_CHECK(!store.remove("alice")); BOOST_CHECK(!std::filesystem::exists(temp.config()));
    for (auto name : {"", "Alice", "../a", "a/b", ".", "a.b", "1abc"}) BOOST_CHECK(!store.add(name,origin()));
    Environment xdg("XDG_CONFIG_HOME","relative"); Environment home("HOME","/home/test"); BOOST_CHECK(!ProfileStore::resolveDirectory());
    auto path = ProfileStore::resolveDirectory(temp.config()); BOOST_REQUIRE(path); BOOST_CHECK_EQUAL(path.value(),temp.config());
    { Environment valid("XDG_CONFIG_HOME","/tmp/config"); BOOST_CHECK_EQUAL(ProfileStore::resolveDirectory().value(),"/tmp/config/anof"); }
    { Environment empty("XDG_CONFIG_HOME",""); BOOST_CHECK_EQUAL(ProfileStore::resolveDirectory().value(),"/home/test/.config/anof"); }
    BOOST_CHECK(!ProfileStore::resolveDirectory("/tmp/../unsafe"));
}
BOOST_AUTO_TEST_CASE(atomic_profiles_redaction_and_stable_locks) {
    Temp temp; ProfileStore store(temp.config()); BOOST_REQUIRE(store.add("alice",origin())); BOOST_REQUIRE(store.add("bob",Origin::parse("https://example.com",false).value()));
    const auto profiles = std::filesystem::path(temp.config())/"profiles";
    BOOST_CHECK_EQUAL(mode(temp.config()),0700); BOOST_CHECK_EQUAL(mode(profiles),0700); BOOST_CHECK_EQUAL(mode(profiles/"alice.json"),0600);
    BOOST_CHECK(!store.add("alice",origin()));
    {
        auto held = store.lock("alice"); BOOST_REQUIRE(held); auto blocked = store.lock("alice"); BOOST_REQUIRE(!blocked); BOOST_CHECK_EQUAL(blocked.error().code,"PROFILE_BUSY");
        BOOST_CHECK(store.lock("bob")); BOOST_CHECK(!store.list()); BOOST_CHECK(!store.remove("alice"));
        BOOST_REQUIRE(held.value().saveCredential(credential())); BOOST_CHECK(held.value().summary().session);
        auto before = read(profiles/"alice.json"); BOOST_CHECK(before.find(token) != std::string::npos);
        BOOST_CHECK(!store.lock("alice")); // 原子替换没有释放稳定锁。
    }
    auto shown = store.show("bob"); BOOST_REQUIRE(shown); BOOST_CHECK(!shown.value().session);
    auto listed = store.list(); BOOST_REQUIRE(listed); BOOST_CHECK_EQUAL(listed.value().size(),2); BOOST_CHECK_EQUAL(listed.value()[0].name,"alice");
    BOOST_REQUIRE(store.remove("alice")); BOOST_CHECK(std::filesystem::exists(profiles/".alice.lock")); BOOST_REQUIRE(store.add("alice",origin())); BOOST_CHECK(!store.show("alice").value().session);
}
BOOST_AUTO_TEST_CASE(reject_unsafe_objects_without_fixing_them) {
    Temp temp; ProfileStore store(temp.config()); BOOST_REQUIRE(store.add("alice",origin())); const auto profiles = std::filesystem::path(temp.config())/"profiles"; const auto file = profiles/"alice.json";
    BOOST_REQUIRE(chmod(file.c_str(),0644)==0); BOOST_CHECK(!store.show("alice")); BOOST_CHECK_EQUAL(mode(file),0644); BOOST_REQUIRE(chmod(file.c_str(),0600)==0);
    BOOST_REQUIRE(link(file.c_str(),(profiles/"alias").c_str())==0); BOOST_CHECK(!store.show("alice")); BOOST_REQUIRE(unlink((profiles/"alias").c_str())==0);
    std::filesystem::rename(file,profiles/"original"); std::filesystem::create_symlink("original",file); BOOST_CHECK(!store.show("alice")); BOOST_CHECK(!store.remove("alice")); std::filesystem::remove(file);
    BOOST_REQUIRE(mkfifo(file.c_str(),0600)==0); BOOST_CHECK(!store.show("alice")); std::filesystem::remove(file); std::filesystem::rename(profiles/"original",file);
    BOOST_REQUIRE(chmod(profiles.c_str(),0755)==0); BOOST_CHECK(!store.list()); BOOST_REQUIRE(chmod(profiles.c_str(),0700)==0);
    std::filesystem::create_directory_symlink(temp.config(),temp.path/"alias"); ProfileStore alias((temp.path/"alias").string()); BOOST_CHECK(!alias.list());
    std::filesystem::rename(profiles/".alice.lock",profiles/"saved-lock"); std::filesystem::create_symlink("saved-lock",profiles/".alice.lock"); BOOST_CHECK(!store.show("alice"));
}
BOOST_AUTO_TEST_CASE(strict_profile_format_and_size) {
    Temp temp; ProfileStore store(temp.config()); BOOST_REQUIRE(store.add("alice",origin())); const auto file = std::filesystem::path(temp.config())/"profiles/alice.json"; const auto original = read(file);
    for (const auto& content : std::vector<std::string>{"", "{", original+"{}", "{\"version\":1,\"version\":1}", std::string(16385,'x')}) { replace(file,content); BOOST_CHECK(!store.show("alice")); BOOST_CHECK(!store.list()); }
    for (auto field : {"version", "server", "development_http", "credential"}) { auto j = Json::parse(original); j.erase(field); replace(file,j.dump()); BOOST_CHECK(!store.show("alice")); }
    auto j = Json::parse(original); j["version"] = 2; replace(file,j.dump()); BOOST_CHECK(!store.show("alice"));
    j = Json::parse(original); j["extra"] = 1; replace(file,j.dump()); BOOST_CHECK(!store.show("alice"));
    j = Json::parse(original); j["server"] = "http://127.0.0.1:08080/"; replace(file,j.dump()); BOOST_CHECK(!store.show("alice"));
    replace(file,original); auto held = store.lock("alice"); BOOST_REQUIRE(held); BOOST_CHECK(held.value().saveCredential(credential()));
}
BOOST_AUTO_TEST_CASE(publication_failures_and_directory_limit) {
    Temp temp; ProfileStore store(temp.config()); BOOST_REQUIRE(store.add("alice",origin())); const auto profiles = std::filesystem::path(temp.config())/"profiles"; const auto original = read(profiles/"alice.json");
    { ProfileStore failing(temp.config(),[](SavePoint p){return p != SavePoint::BeforePublish;}); auto held = failing.lock("alice"); BOOST_REQUIRE(held); auto saved = held.value().saveCredential(credential()); BOOST_REQUIRE(!saved); BOOST_CHECK(saved.error().local == LocalState::Unchanged); }
    BOOST_CHECK_EQUAL(read(profiles/"alice.json"),original);
    { ProfileStore failing(temp.config(),[](SavePoint p){return p != SavePoint::AfterPublish;}); auto held = failing.lock("alice"); BOOST_REQUIRE(held); auto saved = held.value().saveCredential(credential()); BOOST_REQUIRE(!saved); BOOST_CHECK(saved.error().local == LocalState::Unknown); BOOST_CHECK(!held.value().clearCredential()); }
    BOOST_CHECK(store.show("alice").value().session);
    for (const auto& entry : std::filesystem::directory_iterator(profiles)) BOOST_CHECK(entry.path().filename().string().find(".tmp-") != 0);
    for (unsigned i=0; i<99; ++i) BOOST_REQUIRE(store.add("p"+std::to_string(i),origin()));
    BOOST_CHECK(!store.add("overflow",origin())); auto listed = store.list(); BOOST_REQUIRE(listed); BOOST_CHECK_EQUAL(listed.value().size(),100);
    replace(profiles/"invalid.name.json","not a profile"); BOOST_CHECK(store.list());
}
BOOST_AUTO_TEST_CASE(directory_lock_and_corruption_identification) {
    Temp temp; ProfileStore store(temp.config()); BOOST_REQUIRE(store.add("alice",origin())); const auto profiles=std::filesystem::path(temp.config())/"profiles";
    int fd=open((profiles/".directory.lock").c_str(),O_RDWR|O_CLOEXEC); BOOST_REQUIRE(fd>=0); BOOST_REQUIRE(flock(fd,LOCK_EX|LOCK_NB)==0);
    BOOST_CHECK(!store.list()); BOOST_CHECK(!store.add("bob",origin())); BOOST_CHECK(!store.remove("alice")); BOOST_CHECK(store.show("alice"));
    close(fd);
    replace(profiles/"alice.json","{invalid"); auto listed=store.list(); BOOST_REQUIRE(!listed); BOOST_REQUIRE(listed.error().profileName); BOOST_CHECK_EQUAL(*listed.error().profileName,"alice");
}
