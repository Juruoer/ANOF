#define BOOST_TEST_MODULE TransferClientProcess
#include <tests/integration/ClientProcess.h>
using namespace anof::testing::cli;
namespace {
constexpr const char* password = "fixture secret password";
struct Fixture {
    Temp temp;
    std::filesystem::path root = temp.path / "instance";
    std::unique_ptr<Process> server;
    Fixture() {
        std::filesystem::create_directory(root);
        chmod(root.c_str(), 0700);
        Process init({ "init",
                       "--root",
                       root.string(),
                       "--admin-login",
                       "admin",
                       "--admin-display-name",
                       "Administrator",
                       "--development-http",
                       "--content-budget-bytes",
                       "16777216",
                       "--private-bytes",
                       "16777216",
                       "--private-nodes",
                       "100",
                       "--public-bytes",
                       "16777216",
                       "--public-nodes",
                       "100",
                       "--contribution-bytes",
                       "16777216",
                       "--contribution-nodes",
                       "100" },
                     true);
        init.input("Administrator password: ", password);
        init.input("Confirm password: ", password);
        init.finish(0);
        file(root / ".anof/config.toml", "development_http=true\nport=0\n");
        server = std::make_unique<Process>(std::vector<std::string>{ "serve", "--root", root.string() });
        auto port = server->port();
        file(root / ".anof/config.toml", "development_http=true\nport=" + std::to_string(port) + "\n");
        run(temp, "",
            { "profile", "add", "admin", "--server", "http://127.0.0.1:" + std::to_string(port),
              "--development-http" });
        login(temp, "admin", "admin", password);
    }
    ~Fixture() {
        if (server) server->stop();
    }
    Json call(std::vector<std::string> args) {
        Process p(command(temp, "admin", std::move(args)));
        p.finish(0);
        BOOST_REQUIRE_MESSAGE(!p.out.empty(), p.err);
        return Json::parse(p.out)["data"];
    }
    Json upload(const std::string& name, const std::string& bytes) {
        auto source = temp.path / name;
        file(source, bytes);
        Json space;
        const auto spaces = call({ "spaces" });
        for (const auto& s : spaces["items"])
            if (s["kind"] == "private") space = s;
        return call(
            { "upload", source.string(), "--space", space["id"], "--parent", space["root_node_id"], "--name", name });
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(real_cli_upload_download_and_cross_process_resume) {
    Fixture f;
    const std::string bytes(150000, 'x');
    auto result = f.upload("payload", bytes);
    BOOST_REQUIRE(result["published"] == true);
    auto transfer = result["transfer_id"].get<std::string>(), node = result["node_id"].get<std::string>();
    auto again = f.call({ "upload", "resume", transfer });
    BOOST_CHECK(again["node_id"] == node);
    auto show = f.call({ "upload", "show", result["upload_id"] });
    BOOST_CHECK(show["state"] == "COMPLETED");
    BOOST_CHECK_EQUAL(f.call({ "upload", "list", "--limit", "1" })["items"].size(), 1);
    auto output = f.temp.path / "download";
    auto downloaded = f.call({ "download", node, "--output", output.string() });
    std::ifstream input(output);
    std::string actual((std::istreambuf_iterator<char>(input)), {});
    BOOST_CHECK(actual == bytes);
    auto resumed = f.call({ "download", "resume", downloaded["transfer_id"] });
    BOOST_CHECK(resumed["published"] == true);
    // 将自己的完整临时文件及记录恢复到合法中断边界，下一进程必须按持久偏移进行Range。
    const auto id = downloaded["transfer_id"].get<std::string>();
    auto recordPath = f.temp.path / "config/transfers" / (id + ".json");
    auto record = jsonFile(recordPath);
    auto part = f.temp.path / record["part"].get<std::string>();
    std::filesystem::rename(output, part);
    std::filesystem::resize_file(part, 70000);
    record["durable_offset"] = "65536";
    record["phase"] = "RECEIVING";
    file(recordPath, record.dump());
    f.server->stop();
    f.server = std::make_unique<Process>(std::vector<std::string>{ "serve", "--root", f.root.string() });
    f.server->port();
    resumed = f.call({ "download", "resume", id });
    BOOST_CHECK(resumed["published"] == true);
    std::ifstream complete(output);
    actual.assign(std::istreambuf_iterator<char>(complete), {});
    BOOST_CHECK(actual == bytes);
    // 发布后、完成记录保存前的边界由同一inode证据确认，不覆盖目标。
    record = jsonFile(recordPath);
    record["phase"] = "PUBLISHING";
    file(recordPath, record.dump());
    BOOST_CHECK(f.call({ "download", "resume", id })["published"] == true);
    Process exists(command(f.temp, "admin", { "download", node, "--output", output.string() }));
    exists.failure(5);
    file(f.temp.path / "payload", "modified");
    Process changed(command(f.temp, "admin", { "upload", "resume", transfer }));
    BOOST_CHECK(changed.success()["data"]["published"] == true);
    const auto storage = run(f.temp, "admin", { "storage", "show" });
    f.call({ "storage", "set", "--content-budget-bytes", "16777216", "--if-match",
             storage["meta"]["etag"].get<std::string>() });
    BOOST_CHECK(jsonFile(recordPath).dump().find("access_token") == std::string::npos);
}
BOOST_AUTO_TEST_CASE(empty_files_and_safe_path_rejections) {
    Fixture f;
    auto result = f.upload("empty", "");
    BOOST_REQUIRE(result["published"] == true);
    auto output = f.temp.path / "empty-out";
    f.call({ "download", result["node_id"], "--output", output.string() });
    BOOST_CHECK_EQUAL(std::filesystem::file_size(output), 0);
    auto id = result["transfer_id"].get<std::string>();
    auto profilePath = f.temp.path / "config/profiles/admin.json";
    auto p = jsonFile(profilePath);
    p["server"] = "http://127.0.0.1:1";
    file(profilePath, p.dump());
    Process origin(command(f.temp, "admin", { "upload", "resume", id }));
    origin.failure(5);
}

#include <anof/client/CurlHttpTransport.h>
#include <anof/client/ProfileStore.h>
#include <anof/client/internal/TransferFiles.h>
BOOST_AUTO_TEST_CASE(real_partial_upload_is_resumed_by_next_cli_process) {
    namespace c = anof::client;
    namespace files = c::transfer_internal;
    Fixture f;
    const auto source = f.temp.path / "partial-source";
    file(source, "abc");
    std::string transfer;
    Json space;
    const auto spaces = f.call({ "spaces" });
    for (const auto& s : spaces["items"])
        if (s["kind"] == "private") space = s;
    {
        c::ProfileStore store((f.temp.path / "config").string());
        auto held = store.lock("admin");
        BOOST_REQUIRE(held);
        const auto& p = held.value().profile();
        files::Record record((f.temp.path / "config").string(), p);
        transfer = record.id();
        files::Fd fd(open(source.c_str(), O_RDONLY | O_CLOEXEC));
        auto& r = record.data;
        r["kind"] = "upload";
        r["file"] = source.string();
        r["target"] = { { "space_id", space["id"] }, { "parent_id", space["root_node_id"] }, { "name", "partial" } };
        r["source_identity"] = files::identity(fd.value);
        r["sha256"] = files::hash(fd.value, nullptr);
        r["length"] = "3";
        r["client_upload_id"] = files::uuid();
        r["upload_id"] = nullptr;
        r["phase"] = "CREATING";
        record.save();
        Json body = r["target"];
        body["length"] = "3";
        body["sha256"] = r["sha256"];
        body["client_upload_id"] = r["client_upload_id"];
        c::SecureBuffer payload(c::kRequestLimit);
        BOOST_REQUIRE(payload.append(body.dump()));
        c::CurlHttpTransport http;
        auto created = http.perform(
            p.server, { c::Method::Post, "/api/v1/uploads", std::move(payload), &p.credential->accessToken });
        BOOST_REQUIRE(created);
        BOOST_REQUIRE_EQUAL(created.value().status, 201);
        auto upload = Json::parse(created.value().body.view());
        r["upload_id"] = upload["id"];
        r["instance_id"] = upload["instance_id"];
        r["phase"] = "OPEN";
        record.save();
        c::TransferRequest patch{ c::Method::Patch,
                                  "/api/v1/uploads/" + upload["id"].get<std::string>(),
                                  &p.credential->accessToken,
                                  { { "Upload-Offset", "0" } },
                                  1,
                                  {},
                                  {},
                                  {} };
        patch.source = [](char* data, std::size_t) -> c::ClientResult<std::size_t> {
            *data = 'a';
            return 1;
        };
        auto written = http.transfer(p.server, patch);
        BOOST_REQUIRE(written);
        BOOST_CHECK_EQUAL(written.value().status, 204);
        // 本地未更新offset，模拟确认响应丢失后退出；下一进程必须采纳服务端offset=1。
    }
    f.server->stop();
    f.server = std::make_unique<Process>(std::vector<std::string>{ "serve", "--root", f.root.string() });
    f.server->port();
    auto resumed = f.call({ "upload", "resume", transfer });
    BOOST_CHECK(resumed["published"] == true);
    auto destination = f.temp.path / "partial-output";
    f.call({ "download", resumed["node_id"], "--output", destination.string() });
    std::ifstream in(destination);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    BOOST_CHECK_EQUAL(bytes, "abc");
}

BOOST_AUTO_TEST_CASE(real_cli_drive_management_two_users_and_deleted_upload_resume) {
    Fixture f;
    auto uploaded = f.upload("source", "abc");
    auto n = f.call({ "stat", uploaded["node_id"] });
    Json space;
    const auto available = f.call({ "spaces" });
    for (const auto& s : available.at("items")) {
        if (s["kind"] == "private") space = s;
    }
    auto dir = f.call({ "mkdir", "--space", space["id"], "--parent", space["root_node_id"], "--name", "destination" });
    auto renamed = f.call({ "rename", n["id"], "--name", "find%_one", "--if-match", n["etag"] });
    auto moved = f.call({ "mv", n["id"], "--parent", dir["id"], "--if-match", renamed["etag"] });
    Process stale(command(f.temp, "admin", { "rename", n["id"], "--name", "stale", "--if-match", n["etag"] }));
    auto failure = stale.failure(3);
    BOOST_CHECK_EQUAL(failure["code"].get<std::string>(), "PRECONDITION_FAILED");
    BOOST_CHECK_EQUAL(failure["operation"].get<std::string>(), "rename");
    auto copy = f.call({ "cp", n["id"], "--space", space["id"], "--parent", space["root_node_id"], "--name", "find%_two", "--if-match", moved["etag"] });
    auto page = f.call({ "search", "--space", space["id"], "--query", "%_", "--limit", "1" });
    BOOST_REQUIRE(!page["next_cursor"].is_null());
    auto next = f.call({ "search", "--space", space["id"], "--query", "%_", "--limit", "1", "--cursor", page["next_cursor"] });
    BOOST_CHECK(page["items"][0]["id"] != next["items"][0]["id"]);
    auto storage = f.call({ "storage", "show" });
    BOOST_CHECK_EQUAL(storage["retained_bytes"].get<std::string>(), "3");
    BOOST_CHECK_EQUAL(storage["pending_gc_bytes"].get<std::string>(), "0");
    BOOST_CHECK_EQUAL(storage["node_limit"].get<std::string>(), "100000");
    const auto origin = jsonFile(f.temp.path / "config/profiles/admin.json")["server"].get<std::string>();
    auto invitation = f.call({ "invite", "create", "--max-uses", "2" });
    for (const auto* user : { "alice", "bob" }) {
        run(f.temp, "", { "profile", "add", user, "--server", origin, "--development-http" });
        Process reg(command(f.temp, user, { "register", "--login", user, "--display-name", user }), true);
        reg.input("Invitation token: ", invitation["token"]);
        reg.input("New password: ", password);
        reg.input("Confirm password: ", password);
        reg.success();
        login(f.temp, user, user, password);
    }
    Process hidden(command(f.temp, "alice", { "rm", moved["id"], "--if-match", moved["etag"] }));
    BOOST_CHECK_EQUAL(hidden.failure(3)["code"].get<std::string>(), "NOT_FOUND");
    Json common;
    auto spaces = run(f.temp, "alice", { "spaces" })["data"];
    for (const auto& s : spaces["items"])
        if (s["kind"] == "public") common = s;
    auto shared = run(f.temp, "alice", { "mkdir", "--space", common["id"], "--parent", common["root_node_id"], "--name", "mixed" })["data"];
    run(f.temp, "bob", { "mkdir", "--space", common["id"], "--parent", shared["id"], "--name", "bob-child" });
    shared = run(f.temp, "alice", { "stat", shared["id"] })["data"];
    Process forbidden(command(f.temp, "alice", { "rm", shared["id"], "--if-match", shared["etag"] }));
    BOOST_CHECK_EQUAL(forbidden.failure(3)["code"].get<std::string>(), "FORBIDDEN");
    dir = f.call({ "stat", dir["id"] });
    auto t = f.call({ "rm", dir["id"], "--if-match", dir["etag"] });
    BOOST_CHECK_EQUAL(t["node_count"].get<std::string>(), "2");
    auto listed = f.call({ "trash", "list", "--space", space["id"], "--root-node", dir["id"] });
    BOOST_REQUIRE_EQUAL(listed["items"].size(), 1);
    BOOST_CHECK(f.call({ "trash", "show", t["id"] })["etag"] == t["etag"]);
    auto restored = f.call({ "trash", "restore", t["id"], "--name", "restored", "--if-match", t["etag"] });
    t = f.call({ "rm", restored["id"], "--if-match", restored["etag"] });
    Process noConfirm(command(f.temp, "admin", { "trash", "purge", t["id"], "--if-match", t["etag"] }));
    noConfirm.failure(2);
    f.call({ "trash", "show", t["id"] });
    Process purge(command(f.temp, "admin", { "trash", "purge", t["id"], "--if-match", t["etag"], "--confirm-permanent" }, false));
    purge.finish(0);
    BOOST_CHECK(purge.out.find("physical reclamation may remain pending") != std::string::npos);
    auto deleted = f.call({ "upload", "show", uploaded["upload_id"] });
    BOOST_CHECK(!deleted["result_deleted_at"].is_null());
    BOOST_CHECK(f.call({ "upload", "complete", uploaded["upload_id"] })["result_deleted_at"] == deleted["result_deleted_at"]);
    Process resume(command(f.temp, "admin", { "upload", "resume", uploaded["transfer_id"] }));
    auto e = resume.failure(3);
    BOOST_CHECK_EQUAL(e["code"].get<std::string>(), "UPLOAD_RESULT_DELETED");
    auto output = f.temp.path / "copy-download";
    f.call({ "download", copy["id"], "--output", output.string() });
    std::ifstream input(output);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    BOOST_CHECK_EQUAL(bytes, "abc");
    t = f.call({ "rm", copy["id"], "--if-match", copy["etag"] });
    f.call({ "trash", "purge", t["id"], "--if-match", t["etag"], "--confirm-permanent" });
    storage = f.call({ "storage", "show" });
    BOOST_CHECK_EQUAL(storage["pending_gc_bytes"].get<std::string>(), "3");
    BOOST_CHECK_EQUAL(storage["pending_gc_objects"].get<std::string>(), "1");
    f.server->stop();
    f.server = std::make_unique<Process>(std::vector<std::string>{ "serve", "--root", f.root.string() });
    f.server->port();
    BOOST_CHECK(f.call({ "upload", "show", uploaded["upload_id"] })["result_deleted_at"] == deleted["result_deleted_at"]);
}
