#define BOOST_TEST_MODULE HttpDownloadIntegration
#include <anof/transport/http/internal/DownloadState.h>
#include <muduox/net/http/HttpServer.h>
#include <tests/integration/DownloadFixture.h>

namespace anof::http::internal {
struct DownloadStateTestAccess {
    static bool blocked(const DownloadState& s) { return s.waiting_ == DownloadState::Wait::Write; }
    static std::size_t pending(const DownloadState& s) { return s.buffer_ ? s.buffer_->size - s.blockOffset_ : 0; }
};
}  // namespace anof::http::internal
namespace {
std::string body(const std::string& wire) {
    const auto pos = wire.find("\r\n\r\n");
    BOOST_REQUIRE(pos != std::string::npos);
    return wire.substr(pos + 4);
}
std::string header(const std::string& wire, const std::string& name) {
    auto pos = wire.find("\r\n" + name + ": ");
    if (pos == std::string::npos) return {};
    pos += name.size() + 4;
    return wire.substr(pos, wire.find("\r\n", pos) - pos);
}
std::string get(Fixture& f, const Json& upload, const std::string& token, int status,
                const std::string& extra = {}, bool head = false) {
    auto wire = f.exchange(request(head ? "HEAD" : "GET", "/api/v1/nodes/" + node(upload).toString() + "/content", nullptr, token, extra));
    BOOST_REQUIRE_MESSAGE(wire.find("HTTP/1.1 " + std::to_string(status) + " ") == 0, wire);
    return wire;
}
}  // namespace
BOOST_AUTO_TEST_CASE(real_content_conditions_ranges_and_restart) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = publish(f, token);
    auto wire = get(f, upload, token, 200);
    BOOST_CHECK_EQUAL(body(wire), "abc");
    const auto tag = header(wire, "etag");
    BOOST_CHECK_EQUAL(tag, "\"sha256-ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"");
    BOOST_CHECK_EQUAL(header(wire, "cache-control"), "private,no-store");
    BOOST_CHECK(!header(wire, "content-disposition").empty());
    BOOST_CHECK(header(wire, "last-modified").empty());
    wire = get(f, upload, token, 206, "Range: bytes=1-2\r\n");
    BOOST_CHECK_EQUAL(body(wire), "bc");
    BOOST_CHECK_EQUAL(header(wire, "content-range"), "bytes 1-2/3");
    wire = get(f, upload, token, 200, "Range: bytes=1-2\r\n", true);
    BOOST_CHECK(body(wire).empty());
    BOOST_CHECK(wire.find("Content-Length: 3\r\n") != std::string::npos);
    wire = get(f, upload, token, 304, "If-None-Match: " + tag + "\r\n");
    BOOST_CHECK(body(wire).empty());
    get(f, upload, token, 412, "If-Match: \"different\"\r\n");
    get(f, upload, token, 400, "If-Match: invalid\r\n");
    wire = get(f, upload, token, 416, "Range: bytes=3-\r\n");
    BOOST_CHECK_EQUAL(header(wire, "content-range"), "bytes */3");
    BOOST_CHECK(body(wire).empty());
    BOOST_CHECK_EQUAL(body(get(f, upload, token, 200, "Range: bytes=1-\r\nIf-Range: \"different\"\r\n")), "abc");
    BOOST_CHECK_EQUAL(body(get(f, upload, token, 200, "Range: bytes=0-0,2-2\r\n")), "abc");
    f.stop();
    f.start();
    BOOST_CHECK_EQUAL(body(get(f, upload, token, 200)), "abc");
}
BOOST_AUTO_TEST_CASE(authorization_and_object_validation_precede_conditions) {
    Fixture f;
    f.start();
    auto admin = f.login();
    auto invite = f.call("POST", "/api/v1/admin/invites", 201, Json::object(), admin);
    f.call("POST", "/api/v1/auth/register", 201, registration("alice", invite["token"]));
    auto alice = f.login("alice");
    auto upload = publish(f, alice);
    for (bool head : { false, true }) {
        auto wire = get(f, upload, admin, 404, "If-None-Match: *\r\n", head);
        BOOST_CHECK(header(wire, "etag").empty());
        get(f, upload, "", 401, {}, head);
    }
    auto opened = std::optional<Result<OpenedDownload>>{};
    auto task = f.server->transferDispatcher().submit(context(alice), OpenDownloadCommand{ node(upload), true },
                                                      [&](TaskCompletion<OpenedDownload> c) { opened = std::move(std::get<Result<OpenedDownload>>(c.outcome)); });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(task));
    BOOST_REQUIRE(drive(f.loop, [&] { return opened.has_value(); }));
    BOOST_REQUIRE(*opened);
    std::filesystem::resize_file(f.root / ".anof" / opened->value().descriptor.blob.storageKey(), 1);
    for (bool head : { false, true }) {
        auto wire = get(f, upload, alice, 500, "If-None-Match: *\r\n", head);
        BOOST_CHECK(header(wire, "etag").empty());
        if (head) BOOST_CHECK(body(wire).empty());
    }
}
BOOST_AUTO_TEST_CASE(empty_content_and_directory_rejection) {
    Fixture f;
    f.start();
    auto token = f.login();
    auto upload = publish(f, token, true);
    BOOST_CHECK(body(get(f, upload, token, 200, "Range: bytes=0-\r\n")).empty());
    upload["result_node_id"] = upload["parent_id"];
    get(f, upload, token, 409);
}

// 独立组件 listener 限制真实 writer 水位；复用实例的真实 SQLite、磁盘及身份端口。
// 生产 HttpApi 路由由上面的真实实例案例验证，此夹具没有伪造读取或 writer。
BOOST_AUTO_TEST_CASE(real_disk_writer_backpressure_recovery_and_disable_cleanup) {
    using namespace muduox::net;
    using State = anof::http::internal::DownloadState;
    using Access = anof::http::internal::DownloadStateTestAccess;
    constexpr std::size_t length = 8 * 1024 * 1024;
    for (bool cancel : { false, true }) {
        Fixture f(16 * 1024 * 1024);
        f.start({}, false, false, 2);
        auto admin = f.login();
        auto invite = f.call("POST", "/api/v1/admin/invites", 201, Json::object(), admin);
        auto user = f.call("POST", "/api/v1/auth/register", 201, registration("alice", invite["token"]));
        auto token = f.login("alice");
        Json space;
        const auto spaces = f.call("GET", "/api/v1/spaces", 200, nullptr, token);
        for (const auto& item : spaces["items"])
            if (item["kind"] == "private") space = item;
        auto upload = f.call("POST", "/api/v1/uploads", 201,
                             { { "client_upload_id", "10000000-0000-4000-8000-000000000001" }, { "space_id", space["id"] }, { "parent_id", space["root_node_id"] }, { "name", "large" }, { "length", std::to_string(length) } }, token);
        auto url = "/api/v1/uploads/" + upload["id"].get<std::string>();
        const std::string chunk(65536, 'x');
        for (std::size_t offset = 0; offset < length; offset += chunk.size()) {
            auto response = f.exchange("PATCH " + url + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nAuthorization: Bearer " + token +
                                       "\r\nContent-Type: application/offset+octet-stream\r\nContent-Length: 65536\r\nUpload-Offset: " + std::to_string(offset) + "\r\n\r\n" + chunk);
            BOOST_REQUIRE_MESSAGE(response.find("HTTP/1.1 204 ") == 0, response);
        }
        f.call("POST", url + "/complete", 202, Json::object(), token);
        for (unsigned i = 0; i < 300; ++i) {
            upload = f.call("GET", url, 200, nullptr, token);
            if (upload["state"] == "COMPLETED") break;
        }
        BOOST_REQUIRE(upload["state"] == "COMPLETED");
        std::optional<Result<OpenedDownload>> info;
        f.server->transferDispatcher().submit(context(token), OpenDownloadCommand{ node(upload), true },
                                              [&](TaskCompletion<OpenedDownload> c) { info = std::move(std::get<Result<OpenedDownload>>(c.outcome)); });
        BOOST_REQUIRE(drive(f.loop, [&] { return info.has_value(); }));
        BOOST_REQUIRE(*info);
        auto blob = info->value().descriptor.blob.id;
        auto& executor = dynamic_cast<UploadExecutor&>(f.server->transferDispatcher());
        {
            HttpServer server(&f.loop, InetAddress(0, true), "real-disk-download");
            std::weak_ptr<State> state;
            server.setHttpStreamCallback([&](const auto& input, const auto& writer) {
                auto value = std::make_shared<State>(f.server->transferDispatcher(), context(token), OpenDownloadCommand{ node(upload), false }, input.headers(), [] { return true; }, writer);
                state = value;
                return value->callbacks();
            });
            HttpOutputLimits limits;
            limits.highWaterBytes = 65536;
            server.setOutputLimits(limits);
            server.start();
            struct Stop {
                HttpServer& server;
                Fixture& fixture;
                ~Stop() {
                    bool stopped = false;
                    server.stop(0, [&] { stopped = true; });
                    if (!drive(fixture.loop, [&] { return stopped; })) std::terminate();
                }
            } stop{ server, f };
            Client client(server.listenAddress().port());
            client.limit = length + 65536;
            client.send(request("GET", "/content"));
            std::size_t observedPending = 0;
            // quit不会撤销同批回调；在同一次观察中保存背压及剩余量，不能在loop返回后重读瞬态状态。
            BOOST_REQUIRE(drive(f.loop, [&] {
                auto s = state.lock();
                if (!s || !Access::blocked(*s)) return false;
                observedPending = Access::pending(*s);
                return observedPending > 0;
            }));
            BOOST_CHECK(observedPending <= 65536);
            BOOST_CHECK(executor.blobPinned(blob));
            if (cancel) {
                const auto tag = "If-Match: \"user-" + user["id"].get<std::string>() + "-" + user["revision"].get<std::string>() + "\"\r\n";
                f.call("PATCH", "/api/v1/admin/users/" + user["id"].get<std::string>(), 200, { { "enabled", false } }, admin, tag);
            }
            BOOST_REQUIRE(drive(f.loop, [&] { client.read(); return client.eof && !executor.blobPinned(blob); }));
            BOOST_REQUIRE(client.bytes.find("HTTP/1.1 200 ") == 0);
            if (cancel)
                BOOST_CHECK(body(client.bytes).size() < length);
            else
                BOOST_CHECK_EQUAL(body(client.bytes), std::string(length, 'x'));
            f.locked();
        }
        if (!cancel) {
            // 正式 HttpApi 接纳下载后停止：grace 保留读取，零 grace 强制取消。
            for (bool graceful : { true, false }) {
                Client active(*f.server->port());
                active.limit = length + 65536;
                active.send(request("GET", "/api/v1/nodes/" + node(upload).toString() + "/content", nullptr, token));
                BOOST_REQUIRE(drive(f.loop, [&] { return dynamic_cast<UploadExecutor&>(f.server->transferDispatcher()).blobPinned(blob); }));
                f.server->requestStop();
                f.locked();
                BOOST_REQUIRE(drive(f.loop, [&] {
                    active.read();
                    return active.eof && f.server->state() == RuntimeState::Stopped;
                }));
                BOOST_CHECK(!dynamic_cast<UploadExecutor&>(f.server->transferDispatcher()).blobPinned(blob));
                f.locked();
                if (graceful) {
                    BOOST_CHECK_EQUAL(body(active.bytes), std::string(length, 'x'));
                    f.stop();
                    f.start();
                } else {
                    BOOST_CHECK(active.bytes.size() < length);
                }
            }
        }
    }
}
