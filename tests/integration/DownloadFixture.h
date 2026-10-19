#pragma once
#include <tests/integration/HttpInstanceFixture.h>
namespace {
RequestContext context(const std::string& token) {
    auto codec = crypto::makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    auto digest = codec.value()->digestBearer(token);
    BOOST_REQUIRE(digest);
    return { { digest.value(), CredentialMode::Bearer }, RequestId::parse("10000000-0000-4000-8000-000000000001").value() };
}
Json publish(Fixture& f, const std::string& token, bool empty = false) {
    const auto spaces = f.call("GET", "/api/v1/spaces", 200, nullptr, token);
    Json space;
    for (const auto& item : spaces["items"])
        if (item["kind"] == "private") space = item;
    auto upload = f.call("POST", "/api/v1/uploads", 201,
                         { { "client_upload_id", "10000000-0000-4000-8000-000000000001" }, { "space_id", space["id"] }, { "parent_id", space["root_node_id"] }, { "name", "payload" }, { "length", empty ? "0" : "3" } }, token);
    const auto url = "/api/v1/uploads/" + upload["id"].get<std::string>();
    if (!empty) {
        const auto result = f.exchange("PATCH " + url + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nAuthorization: Bearer " + token +
                                       "\r\nContent-Type: application/offset+octet-stream\r\nContent-Length: 3\r\nUpload-Offset: 0\r\n\r\nabc");
        BOOST_REQUIRE(result.find("HTTP/1.1 204 ") == 0);
    }
    f.call("POST", url + "/complete", 202, Json::object(), token);
    for (unsigned i = 0; i < 200; ++i) {
        upload = f.call("GET", url, 200, nullptr, token);
        if (upload["state"] == "COMPLETED") return upload;
    }
    throw std::runtime_error("publish incomplete");
}
NodeId node(const Json& upload) { return NodeId::parse(upload["result_node_id"].get<std::string>()).value(); }
}  // namespace
