#pragma once
#include <anof/client/ApiClient.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <nlohmann/json.hpp>
namespace anof::client::test {
using Json = nlohmann::json;
inline const std::string uid = "10000000-0000-4000-8000-000000000001";
inline const std::string nid = "20000000-0000-4000-8000-000000000001";
inline const std::string sid = "30000000-0000-4000-8000-000000000001";
inline const std::string rid = "40000000-0000-4000-8000-000000000001";
inline const std::string timestamp = "2026-10-11T10:00:00.000000Z";
inline const std::string token(43, 'A');
inline Json userJson() { return { { "id", uid }, { "login", "admin" }, { "display_name", "家庭管理员" }, { "role", "administrator" }, { "enabled", true }, { "revision", "9223372036854775807" }, { "created_at", timestamp }, { "updated_at", timestamp } }; }
inline Json nodeJson() { return { { "id", nid }, { "space_id", sid }, { "parent_id", nid }, { "name", "照片" }, { "created_by", uid }, { "created_at", timestamp }, { "updated_at", timestamp }, { "etag", "\"node-" + nid + "-1\"" }, { "kind", "directory" } }; }
inline Json inviteJson() { return { { "id", rid }, { "created_by", uid }, { "created_at", timestamp }, { "expires_at", "2026-10-12T10:00:00.000000Z" }, { "max_uses", 2 }, { "used_count", 0 }, { "revoked_at", nullptr } }; }
inline HttpResponse rawResponse(int status, std::string_view body) {
    HttpResponse r;
    r.status = status;
    r.headers = { { "content-type", "application/json" }, { "x-request-id", rid } };
    if (!r.body.append(body)) throw std::runtime_error("fixture too large");
    return r;
}
inline HttpResponse response(int status, const Json& value) { return rawResponse(status, value.dump()); }
inline HttpResponse loginResponse() { return response(201, { { "user", userJson() }, { "expires_at", "2026-10-18T10:00:00.000000Z" }, { "token_type", "Bearer" }, { "access_token", token } }); }
inline HttpResponse rejection(int status, const std::string& code) { return response(status, { { "error", { { "code", code }, { "message", "untrusted secret message" }, { "details", Json::object() }, { "request_id", rid } } } }); }
inline StoredCredential credential() { return { SecretToken::session(token).value(), timestamp, UserId::parse(uid).value(), LoginName::parse("admin").value() }; }
struct Temp {
    std::filesystem::path path;
    Temp() {
        char pattern[] = "/tmp/anof-client-XXXXXX";
        auto* p = mkdtemp(pattern);
        if (!p) throw std::runtime_error("temporary directory failed");
        path = p;
    }
    ~Temp() {
        std::error_code e;
        std::filesystem::remove_all(path, e);
    }
    std::string config() const { return (path / "config").string(); }
};
struct MockTransport : HttpTransport {
    unsigned calls = 0;
    std::function<ClientResult<HttpResponse>(const Origin&, const HttpRequest&)> reply;
    ClientResult<HttpResponse> perform(const Origin& o, const HttpRequest& r, const std::atomic_bool*) override {
        ++calls;
        return reply(o, r);
    }
};
}  // namespace anof::client::test
