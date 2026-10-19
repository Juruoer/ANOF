#define BOOST_TEST_MODULE HttpUploadIntegration
#include <tests/integration/HttpInstanceFixture.h>

#include <fstream>

namespace {
Json privateSpace(Fixture& f, const std::string& token) {
    const auto spaces = f.call("GET", "/api/v1/spaces", 200, nullptr, token);
    for (const auto& space : spaces["items"])
        if (space["kind"] == "private") return space;
    throw std::runtime_error("private space missing");
}
Json creation(const Json& space, unsigned key, std::size_t length, std::string name = "file") {
    char uuid[37];
    std::snprintf(uuid, sizeof uuid, "10000000-0000-4000-8000-%012x", key);
    return { { "client_upload_id", uuid }, { "space_id", space["id"] }, { "parent_id", space["root_node_id"] }, { "name", name }, { "length", std::to_string(length) } };
}
std::string path(const Json& upload) { return "/api/v1/uploads/" + upload["id"].get<std::string>(); }
std::string patchHead(const Json& upload, const std::string& token, unsigned offset, std::size_t count) {
    return "PATCH " + path(upload) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nAuthorization: Bearer " + token +
           "\r\nContent-Type: application/offset+octet-stream\r\nUpload-Offset: " + std::to_string(offset) +
           "\r\nContent-Length: " + std::to_string(count) + "\r\n\r\n";
}
void status(const std::string& wire, int expected) {
    BOOST_REQUIRE_MESSAGE(wire.find("HTTP/1.1 " + std::to_string(expected) + " ") == 0, wire);
}
Json payload(const std::string& wire) { return Json::parse(wire.substr(wire.find("\r\n\r\n") + 4)); }
Json awaitState(Fixture& f, const Json& upload, const std::string& token, const std::string& state) {
    // 有界状态观察；不重发写请求，不以重跑失败用例掩盖问题。
    for (unsigned i = 0; i < 200; ++i) {
        auto value = f.call("GET", path(upload), 200, nullptr, token);
        if (value["state"] == state) return value;
    }
    throw std::runtime_error("upload state did not converge");
}
std::filesystem::path temporary(Fixture& f, const Json& upload) {
    return f.root / ".anof/uploads" / (upload["id"].get<std::string>() + ".part");
}
void prefixWritten(Fixture& f, const Json& upload) {
    BOOST_REQUIRE(drive(f.loop, [&] {
        std::error_code error;
        auto size = std::filesystem::file_size(temporary(f, upload), error);
        return !error && size == 1;
    }));
}
}  // namespace

BOOST_AUTO_TEST_CASE(upload_http_publishes_real_bytes_accounting_and_survives_restart) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto space = privateSpace(f, token);
    std::string bytes(100000, 'x');
    bytes.front() = 'a';
    bytes.back() = 'z';
    auto input = creation(space, 1, bytes.size());
    auto upload = f.call("POST", "/api/v1/uploads", 201, input, token);
    BOOST_CHECK(f.call("POST", "/api/v1/uploads", 200, input, token)["id"] == upload["id"]);
    auto storage = f.call("GET", "/api/v1/admin/storage", 200, nullptr, token);
    BOOST_CHECK_EQUAL(storage["reserved_bytes"].get<std::string>(), "100000");
    auto head = f.exchange(request("HEAD", path(upload), nullptr, token));
    status(head, 200);
    BOOST_CHECK(head.find("upload-offset: 0\r\n") != std::string::npos);
    BOOST_CHECK(head.find("content-type:") == std::string::npos);
    BOOST_CHECK(head.find("content-length:") == std::string::npos);
    BOOST_CHECK(head.substr(head.find("\r\n\r\n") + 4).empty());
    auto patched = f.exchange(patchHead(upload, token, 0, bytes.size()) + bytes);
    status(patched, 204);
    BOOST_CHECK(patched.find("upload-offset: 100000\r\n") != std::string::npos);
    auto mismatch = f.exchange(patchHead(upload, token, 0, 1) + "x");
    status(mismatch, 409);
    BOOST_CHECK_EQUAL(payload(mismatch)["error"]["details"]["offset"].get<std::string>(), "100000");
    f.stop();
    f.start();
    BOOST_CHECK_EQUAL(f.call("GET", path(upload), 200, nullptr, token)["offset"].get<std::string>(), "100000");
    f.call("POST", path(upload) + "/complete", 202, Json::object(), token);
    auto completed = awaitState(f, upload, token, "COMPLETED");
    BOOST_CHECK(f.call("POST", path(upload) + "/complete", 200, Json::object(), token)["result_node_id"] == completed["result_node_id"]);
    auto node = f.call("GET", "/api/v1/nodes/" + completed["result_node_id"].get<std::string>(), 200, nullptr, token);
    BOOST_CHECK_EQUAL(node["kind"].get<std::string>(), "file");
    storage = f.call("GET", "/api/v1/admin/storage", 200, nullptr, token);
    BOOST_CHECK_EQUAL(storage["retained_bytes"].get<std::string>(), "100000");
    BOOST_CHECK_EQUAL(storage["reserved_bytes"].get<std::string>(), "0");
    auto after = privateSpace(f, token);
    BOOST_CHECK_EQUAL(after["quota"]["used"]["bytes"].get<std::string>(), "100000");
    BOOST_CHECK_EQUAL(after["quota"]["used"]["nodes"].get<std::string>(), "2");
    f.stop();
    auto resources = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(resources);
    auto& store = dynamic_cast<TransferMetadataStore&>(*resources.value().metadata);
    auto read = store.beginTransferRead();
    BOOST_REQUIRE(read);
    auto blob = read.value()->blobs().findByNode(NodeId::parse(completed["result_node_id"].get<std::string>()).value());
    BOOST_REQUIRE(blob && blob.value());
    BOOST_CHECK_EQUAL(blob.value()->sha256.toString(), completed["actual_sha256"].get<std::string>());
    std::ifstream file(f.root / ".anof" / blob.value()->storageKey(), std::ios::binary);
    std::string actual((std::istreambuf_iterator<char>(file)), {});
    BOOST_CHECK(actual == bytes);
    BOOST_CHECK(!std::filesystem::exists(temporary(f, upload)));
    read.value().reset();
    resources.value().metadata.reset();
    f.start();
    BOOST_CHECK(f.call("GET", path(upload), 200, nullptr, token)["state"] == "COMPLETED");
}

BOOST_AUTO_TEST_CASE(upload_http_isolation_pagination_cancel_and_storage_conditions) {
    Fixture f;
    f.start();
    auto admin = f.login();
    auto invite = f.call("POST", "/api/v1/admin/invites", 201, Json::object(), admin);
    f.call("POST", "/api/v1/auth/register", 201, registration("alice", invite["token"]));
    auto alice = f.login("alice");
    auto space = privateSpace(f, alice);
    auto first = f.call("POST", "/api/v1/uploads", 201, creation(space, 1, 3), alice);
    auto second = f.call("POST", "/api/v1/uploads", 201, creation(space, 2, 4, "other"), alice);
    f.call("GET", path(first), 404, nullptr, admin);
    auto hidden = f.exchange(patchHead(first, admin, 99, 1) + "x");
    status(hidden, 404);
    BOOST_CHECK(payload(hidden)["error"]["details"].empty());
    f.call("GET", "/api/v1/admin/storage", 403, nullptr, alice);
    auto page = f.call("GET", "/api/v1/uploads?limit=1", 200, nullptr, alice);
    BOOST_REQUIRE(!page["next_cursor"].is_null());
    auto cursor = page["next_cursor"].get<std::string>();
    auto next = f.call("GET", "/api/v1/uploads?limit=1&cursor=" + cursor, 200, nullptr, alice);
    BOOST_CHECK(next["items"][0]["id"] != page["items"][0]["id"]);
    f.call("GET", "/api/v1/uploads?cursor=" + cursor, 400, nullptr, admin);
    auto filtered = f.call("GET", "/api/v1/uploads?client_upload_id=" + first["client_upload_id"].get<std::string>(), 200, nullptr, alice);
    BOOST_REQUIRE_EQUAL(filtered["items"].size(), 1);
    BOOST_CHECK(filtered["items"][0]["id"] == first["id"]);
    auto storageWire = f.exchange(request("GET", "/api/v1/admin/storage", nullptr, admin));
    status(storageWire, 200);
    auto tagStart = storageWire.find("etag: ");
    BOOST_REQUIRE(tagStart != std::string::npos);
    tagStart += 6;
    auto tag = "If-Match: " + storageWire.substr(tagStart, storageWire.find("\r\n", tagStart) - tagStart) + "\r\n";
    f.call("PUT", "/api/v1/admin/storage", 428, { { "content_budget_bytes", "100" } }, admin);
    f.call("PUT", "/api/v1/admin/storage", 403, { { "content_budget_bytes", "100" } }, alice, tag);
    f.call("PUT", "/api/v1/admin/storage", 409, { { "content_budget_bytes", "6" } }, admin, tag);
    f.call("PUT", "/api/v1/admin/storage", 200, { { "content_budget_bytes", "100" } }, admin, tag);
    f.call("PUT", "/api/v1/admin/storage", 412, { { "content_budget_bytes", "200" } }, admin, tag);
    f.call("DELETE", path(first), 204, nullptr, alice);
    f.call("DELETE", path(first), 204, nullptr, alice);
    f.call("DELETE", path(second), 204, nullptr, alice);
    f.stop();
    f.start();
    BOOST_CHECK_EQUAL(f.call("GET", "/api/v1/admin/storage", 200, nullptr, admin)["reserved_bytes"].get<std::string>(), "0");
    BOOST_CHECK(!std::filesystem::exists(temporary(f, first)));
    status(f.exchange(patchHead(first, alice, 0, 1) + "x"), 410);
}

BOOST_AUTO_TEST_CASE(upload_http_unknown_patch_commit_recovers_authoritative_offset) {
    for (bool committed : { false, true }) {
        Fixture f;
        f.start({}, false, true);
        auto token = f.login();
        auto upload = f.call("POST", "/api/v1/uploads", 201, creation(privateSpace(f, token), 1, 3), token);
        f.transferFault.afterCommit = committed;
        f.transferFault.armed = true;
        auto result = f.exchange(patchHead(upload, token, 0, 3) + "abc");
        status(result, 500);
        BOOST_CHECK_EQUAL(payload(result)["error"]["code"].get<std::string>(), "COMMIT_OUTCOME_UNKNOWN");
        BOOST_CHECK_EQUAL(f.transferFault.calls.load(), 1);
        auto state = f.call("GET", path(upload), 200, nullptr, token);
        BOOST_CHECK_EQUAL(state["offset"].get<std::string>(), committed ? "3" : "0");
        f.stop();
        f.start();
        BOOST_CHECK_EQUAL(std::filesystem::file_size(temporary(f, upload)), committed ? 3 : 0);
        BOOST_CHECK_EQUAL(f.call("GET", path(upload), 200, nullptr, token)["offset"].get<std::string>(), committed ? "3" : "0");
    }
}

BOOST_AUTO_TEST_CASE(upload_http_grace_finishes_patch_before_worker_exit_and_unlock) {
    Fixture f;
    f.start({}, false, false, 1);
    auto token = f.login();
    auto upload = f.call("POST", "/api/v1/uploads", 201, creation(privateSpace(f, token), 1, 3), token);
    Client client(*f.server->port());
    client.send(patchHead(upload, token, 0, 3) + "a");
    prefixWritten(f, upload);
    f.server->requestStop();
    f.locked();
    client.send("bc");
    BOOST_REQUIRE(drive(f.loop, [&] { client.read(); return client.eof; }));
    status(client.bytes, 204);
    BOOST_REQUIRE(drive(f.loop, [&] { return f.server->state() == RuntimeState::Stopped; }));
    f.locked();
    f.server.reset();
    f.start();
    BOOST_CHECK_EQUAL(f.call("GET", path(upload), 200, nullptr, token)["offset"].get<std::string>(), "3");
}

BOOST_AUTO_TEST_CASE(upload_http_forced_stop_truncates_unconfirmed_tail_and_resumes) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = f.call("POST", "/api/v1/uploads", 201, creation(privateSpace(f, token), 1, 3), token);
    Client client(*f.server->port());
    client.send(patchHead(upload, token, 0, 3) + "a");
    prefixWritten(f, upload);
    f.server->requestStop();
    f.locked();
    BOOST_REQUIRE(drive(f.loop, [&] { client.read(); return client.eof; }));
    BOOST_CHECK(client.bytes.find("204") == std::string::npos);
    BOOST_REQUIRE(drive(f.loop, [&] { return f.server->state() == RuntimeState::Stopped; }));
    BOOST_CHECK_EQUAL(std::filesystem::file_size(temporary(f, upload)), 0);
    f.locked();
    f.server.reset();
    f.start();
    BOOST_CHECK_EQUAL(f.call("GET", path(upload), 200, nullptr, token)["offset"].get<std::string>(), "0");
    status(f.exchange(patchHead(upload, token, 0, 3) + "abc"), 204);
}

BOOST_AUTO_TEST_CASE(upload_http_per_user_stream_limit_is_503) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto space = privateSpace(f, token);
    auto a = f.call("POST", "/api/v1/uploads", 201, creation(space, 1, 3, "a"), token);
    auto b = f.call("POST", "/api/v1/uploads", 201, creation(space, 2, 3, "b"), token);
    auto c = f.call("POST", "/api/v1/uploads", 201, creation(space, 3, 3, "c"), token);
    Client one(*f.server->port()), two(*f.server->port());
    one.send(patchHead(a, token, 0, 3) + "a");
    prefixWritten(f, a);
    two.send(patchHead(b, token, 0, 3) + "b");
    prefixWritten(f, b);
    auto denied = f.exchange(patchHead(c, token, 0, 1) + "c");
    status(denied, 503);
    BOOST_CHECK_EQUAL(payload(denied)["error"]["code"].get<std::string>(), "TEMPORARILY_UNAVAILABLE");
    f.stop();
}

BOOST_AUTO_TEST_CASE(upload_http_rechecks_revoked_session_before_offset_commit) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = f.call("POST", "/api/v1/uploads", 201, creation(privateSpace(f, token), 1, 3), token);
    Client client(*f.server->port());
    client.send(patchHead(upload, token, 0, 3) + "a");
    prefixWritten(f, upload);
    f.call("DELETE", "/api/v1/auth/session", 204, nullptr, token);
    client.send("bc");
    BOOST_REQUIRE(drive(f.loop, [&] { client.read(); return client.eof; }));
    status(client.bytes, 401);
    f.stop();
    f.start();
    token = f.login();
    BOOST_CHECK_EQUAL(f.call("GET", path(upload), 200, nullptr, token)["offset"].get<std::string>(), "0");
    BOOST_CHECK_EQUAL(std::filesystem::file_size(temporary(f, upload)), 0);
    status(f.exchange(patchHead(upload, token, 0, 3) + "abc"), 204);
    f.call("POST", path(upload) + "/complete", 202, Json::object(), token);
    const auto completed = awaitState(f, upload, token, "COMPLETED");
    BOOST_CHECK_EQUAL(completed["actual_sha256"].get<std::string>(),
                      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
