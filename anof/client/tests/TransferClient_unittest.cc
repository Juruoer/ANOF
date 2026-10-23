#define BOOST_TEST_MODULE TransferClient
#include <anof/client/TransferClient.h>
#include <anof/client/internal/TransferFiles.h>
#include <anof/client/tests/Fixtures.h>

#include <boost/test/unit_test.hpp>
#include <fstream>
using namespace anof;
using namespace anof::client;
using namespace anof::client::test;
namespace {
constexpr const char* sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
struct Binary : TransferTransport {
    std::function<ClientResult<HttpResponse>(const TransferRequest&)> reply;
    unsigned calls = 0;
    ClientResult<HttpResponse> transfer(const Origin&, const TransferRequest& r, const std::atomic_bool*) override {
        ++calls;
        return reply(r);
    }
};
struct Fixture {
    Temp temp;
    ProfileStore store{ temp.config() };
    MockTransport http;
    Binary binary;
    std::optional<LockedProfile> held;
    Fixture() {
        BOOST_REQUIRE(store.add("admin", Origin::parse("http://127.0.0.1:8080", true).value()));
        auto lock = store.lock("admin");
        BOOST_REQUIRE(lock);
        held.emplace(std::move(lock).value());
        BOOST_REQUIRE(held->saveCredential(credential()));
        http.reply = [](const auto&, const auto&) {
            auto r = response(200, userJson());
            r.headers["etag"] = "\"user-" + uid + "-9223372036854775807\"";
            return r;
        };
    }
    TransferClient client() { return { http, binary, *held, temp.config() }; }
    std::filesystem::path record(const std::string& id) {
        return std::filesystem::path(temp.config()) / "transfers" / (id + ".json");
    }
};
HttpResponse head() {
    auto r = rawResponse(200, "");
    r.headers["content-type"] = "application/octet-stream";
    r.headers["content-length"] = "3";
    r.headers["etag"] = "\"sha256-" + std::string(sha) + "\"";
    return r;
}
void save(const std::filesystem::path& p, const Json& j) {
    std::ofstream f(p);
    f << j.dump();
}
Json read(const std::filesystem::path& p) {
    std::ifstream f(p);
    return Json::parse(f);
}
}  // namespace
BOOST_AUTO_TEST_CASE(download_interruption_range_ignored_digest_and_publication) {
    for (bool corrupt : { false, true }) {
        Fixture f;
        auto output = f.temp.path / "out";
        f.binary.reply = [&](const TransferRequest& req) -> ClientResult<HttpResponse> {
            if (req.method == Method::Head) return head();
            auto r = head();
            BOOST_REQUIRE(req.validate(r));
            BOOST_REQUIRE(req.sink("a"));
            return failure(ErrorKind::Transport);
        };
        auto first = f.client().download(NodeId::parse(nid).value(), output.string());
        BOOST_REQUIRE(!first);
        BOOST_REQUIRE(first.error().transferId);
        auto id = *first.error().transferId;
        auto saved = read(f.record(id));
        BOOST_CHECK_EQUAL(saved["durable_offset"].get<std::string>(), "1");
        f.binary.reply = [&](const TransferRequest& req) -> ClientResult<HttpResponse> {
            if (req.method == Method::Head) return head();
            BOOST_CHECK_EQUAL(req.headers.at("Range"), "bytes=1-");
            auto r = head();
            auto validated = req.validate(r);
            BOOST_REQUIRE(!validated);
            return validated.error();
        };
        auto ignored = f.client().resume(id, true);
        BOOST_REQUIRE(!ignored);
        BOOST_CHECK(ignored.error().kind == ErrorKind::Protocol);
        BOOST_CHECK_EQUAL(std::filesystem::file_size(f.temp.path / saved["part"].get<std::string>()), 1);
        f.binary.reply = [&](const TransferRequest& req) -> ClientResult<HttpResponse> {
            if (req.method == Method::Head) return head();
            auto r = head();
            r.status = 206;
            r.headers["content-length"] = "2";
            r.headers["content-range"] = "bytes 1-2/3";
            auto valid = req.validate(r);
            if (!valid) return valid.error();
            auto written = req.sink(corrupt ? "xx" : "bc");
            if (!written) return written.error();
            return r;
        };
        auto finished = f.client().resume(id, true);
        if (corrupt) {
            BOOST_REQUIRE(!finished);
            BOOST_CHECK_EQUAL(finished.error().code, "CHECKSUM_MISMATCH");
            BOOST_CHECK(!std::filesystem::exists(output));
        } else {
            BOOST_REQUIRE(finished);
            BOOST_CHECK(std::filesystem::exists(output));
            saved = read(f.record(id));
            saved["phase"] = "PUBLISHING";
            save(f.record(id), saved);
            auto recovered = f.client().resume(id, true);
            BOOST_REQUIRE(recovered);
            BOOST_CHECK(recovered.value().local == LocalState::FilePublished);
        }
    }
}
BOOST_AUTO_TEST_CASE(unknown_create_uses_persisted_key_without_second_post) {
    Fixture f;
    auto source = f.temp.path / "source";
    {
        std::ofstream o(source);
        o << "abc";
    }
    auto me = f.http.reply;
    unsigned posts = 0;
    std::string key;
    f.http.reply = [&](const auto& o, const HttpRequest& r) -> ClientResult<HttpResponse> {
        if (r.path == "/api/v1/users/me") return me(o, r);
        if (r.method == Method::Post) {
            ++posts;
            auto body = Json::parse(r.body.view());
            key = body["client_upload_id"];
            bool found = false;
            for (const auto& entry :
                 std::filesystem::directory_iterator(std::filesystem::path(f.temp.config()) / "transfers")) {
                if (entry.path().extension() == ".json") {
                    auto record = read(entry.path());
                    BOOST_CHECK(record["client_upload_id"] == key);
                    found = true;
                }
            }
            BOOST_CHECK(found);
            return failure(ErrorKind::OutcomeUnknown);
        }
        BOOST_CHECK(r.path.find("client_upload_id=" + key) != std::string::npos);
        return response(200, { { "items", Json::array() }, { "next_cursor", nullptr } });
    };
    auto first = f.client().upload(source.string(), SpaceId::parse(sid).value(), NodeId::parse(nid).value(), "test");
    BOOST_REQUIRE(!first);
    BOOST_CHECK(first.error().kind == ErrorKind::OutcomeUnknown);
    BOOST_REQUIRE(first.error().transferId);
    auto resumed = f.client().resume(*first.error().transferId, false);
    BOOST_REQUIRE(!resumed);
    BOOST_CHECK_EQUAL(resumed.error().code, "UPLOAD_CREATE_UNRESOLVED");
    BOOST_CHECK_EQUAL(posts, 1);

}
BOOST_AUTO_TEST_CASE(part_replacement_and_record_binding_rejected) {
    Fixture f;
    f.binary.reply = [](const TransferRequest& r) -> ClientResult<HttpResponse> {
        if (r.method == Method::Head) return head();
        return failure(ErrorKind::Transport);
    };
    auto first = f.client().download(NodeId::parse(nid).value(), (f.temp.path / "out").string());
    BOOST_REQUIRE(!first);
    auto id = *first.error().transferId;
    auto record = read(f.record(id));
    auto part = f.temp.path / record["part"].get<std::string>();
    std::filesystem::rename(part, f.temp.path / "old");
    {
        std::ofstream out(part);
        out << "";
    }
    chmod(part.c_str(), 0600);
    auto swapped = f.client().resume(id, true);
    BOOST_REQUIRE(!swapped);
    BOOST_CHECK_EQUAL(swapped.error().code, "PART_REPLACED");
    record["origin"] = "http://127.0.0.1:9999";
    save(f.record(id), record);
    auto mismatch = f.client().resume(id, true);
    BOOST_REQUIRE(!mismatch);
    BOOST_CHECK_EQUAL(mismatch.error().code, "TRANSFER_BINDING_MISMATCH");
}
BOOST_AUTO_TEST_CASE(unknown_patch_queries_server_offset_and_never_replays_confirmed_prefix) {
    Fixture f;
    auto source = f.temp.path / "source";
    {
        std::ofstream o(source);
        o << "abc";
    }
    auto me = f.http.reply;
    unsigned creates = 0, patches = 0;
    Json upload;
    f.http.reply = [&](const auto& origin, const HttpRequest& req) -> ClientResult<HttpResponse> {
        if (req.path == "/api/v1/users/me") return me(origin, req);
        if (req.path == "/api/v1/uploads" && req.method == Method::Post) {
            ++creates;
            auto body = Json::parse(req.body.view());
            upload = { { "id", rid },
                       { "instance_id", sid },
                       { "client_upload_id", body["client_upload_id"] },
                       { "space_id", sid },
                       { "parent_id", nid },
                       { "name", "file" },
                       { "length", "3" },
                       { "offset", "0" },
                       { "expected_sha256", sha },
                       { "actual_sha256", nullptr },
                       { "state", "OPEN" },
                       { "result_node_id", nullptr },
                       { "created_at", timestamp },
                       { "last_progress_at", timestamp },
                       { "expires_at", "2026-10-12T10:00:00.000000Z" },
                       { "terminal_at", nullptr },
                       { "result_deleted_at", nullptr },
                       { "last_error", nullptr },
                       { "max_patch_bytes", 8388608 } };
            auto r = response(201, upload);
            r.headers["location"] = "/api/v1/uploads/" + rid;
            return r;
        }
        if (req.method == Method::Get) return response(200, upload);
        BOOST_CHECK(req.path == "/api/v1/uploads/" + rid + "/complete");
        upload["state"] = "COMPLETED";
        upload["actual_sha256"] = sha;
        upload["result_node_id"] = nid;
        upload["terminal_at"] = timestamp;
        return response(200, upload);
    };
    f.binary.reply = [&](const TransferRequest& req) -> ClientResult<HttpResponse> {
        if (req.method == Method::Head) {
            auto r = rawResponse(200, "");
            r.headers["upload-offset"] = "3";
            r.headers["upload-length"] = "3";
            r.headers["upload-state"] = "OPEN";
            return r;
        }
        ++patches;
        char bytes[64];
        auto read = req.source(bytes, sizeof bytes);
        BOOST_REQUIRE(read);
        BOOST_CHECK_EQUAL(read.value(), 3);
        upload["offset"] = "3";
        return failure(ErrorKind::OutcomeUnknown);
    };
    auto first = f.client().upload(source.string(), SpaceId::parse(sid).value(), NodeId::parse(nid).value(), "file");
    BOOST_REQUIRE(!first);
    BOOST_REQUIRE(first.error().transferId);
    auto recordPath=f.record(*first.error().transferId);auto original=read(recordPath);
    auto changedRecord=original;changedRecord["sha256"]=std::string(64,'0');save(recordPath,changedRecord);
    auto changed=f.client().resume(*first.error().transferId,false);BOOST_REQUIRE(!changed);BOOST_CHECK_EQUAL(changed.error().code,"SOURCE_CHANGED");
    save(recordPath,original);
    auto resumed = f.client().resume(*first.error().transferId, false);
    BOOST_REQUIRE(resumed);
    BOOST_CHECK(resumed.value().data["published"] == true);
    BOOST_CHECK_EQUAL(creates, 1);
    BOOST_CHECK_EQUAL(patches, 1);
    upload["result_deleted_at"]="2026-10-11T10:00:01.000000Z";
    std::filesystem::remove(source);
    auto deleted=f.client().resume(*first.error().transferId,false);
    BOOST_REQUIRE(!deleted);BOOST_CHECK_EQUAL(deleted.error().code,"UPLOAD_RESULT_DELETED");BOOST_CHECK_EQUAL(deleted.error().exitCode(),3);
    BOOST_CHECK_EQUAL(creates,1);BOOST_CHECK_EQUAL(patches,1);
    BOOST_REQUIRE(f.client().show(UploadId::parse(rid).value()));
    auto malformed=upload;malformed.erase("result_deleted_at");auto missing=response(200,malformed);
    BOOST_CHECK(!decodeTransfer(TransferOperation::Show,missing));
    malformed=upload;malformed["result_deleted_at"]="2026-10-11T09:59:59.999999Z";auto backwards=response(200,malformed);
    BOOST_CHECK(!decodeTransfer(TransferOperation::Show,backwards));
    malformed=upload;malformed["state"]="OPEN";malformed["result_node_id"]=nullptr;malformed["terminal_at"]=nullptr;
    auto wrongState=response(200,malformed);BOOST_CHECK(!decodeTransfer(TransferOperation::Show,wrongState));

}
BOOST_AUTO_TEST_CASE(storage_tag_and_read_only_head_failures) {
    BOOST_REQUIRE(ConditionTag::parse("\"storage-" + sid + "-1\""));
    auto r = rawResponse(401, "");
    r.headers.erase("x-request-id");
    auto rejected = decodeTransfer(TransferOperation::ContentHead, r);
    BOOST_REQUIRE(!rejected);
    BOOST_CHECK(rejected.error().kind == ErrorKind::Application);
    BOOST_CHECK_EQUAL(rejected.error().code, "HTTP_REJECTED");
}
BOOST_AUTO_TEST_CASE(publication_conflict_preserves_foreign_target_and_diagnostic_part) {
    Fixture f;
    const auto output = f.temp.path / "out";
    f.binary.reply = [&](const TransferRequest& req) -> ClientResult<HttpResponse> {
        if (req.method == Method::Head) return head();
        auto r = head();
        BOOST_REQUIRE(req.validate(r));
        BOOST_REQUIRE(req.sink("abc"));
        {
            std::ofstream foreign(output);
            foreign << "foreign";
        }
        return r;
    };
    auto result = f.client().download(NodeId::parse(nid).value(), output.string());
    BOOST_REQUIRE(!result);
    BOOST_CHECK_EQUAL(result.error().code, "PUBLISH_CONFLICT");
    BOOST_CHECK(result.error().local == LocalState::Unknown);
    BOOST_REQUIRE(result.error().transferId);
    auto id = *result.error().transferId;
    auto record = read(f.record(id));
    BOOST_CHECK(record["phase"] == "PUBLISHING");
    BOOST_CHECK(std::filesystem::exists(f.temp.path / record["part"].get<std::string>()));
    auto resumed = f.client().resume(id, true);
    BOOST_REQUIRE(!resumed);
    std::ifstream content(output);
    std::string bytes((std::istreambuf_iterator<char>(content)), {});
    BOOST_CHECK_EQUAL(bytes, "foreign");
}
BOOST_AUTO_TEST_CASE(record_permissions_locks_and_capacity_precede_remote_writes) {
    namespace files = anof::client::transfer_internal;
    Fixture f;
    std::string id;
    {
        files::Record first(f.temp.config(), f.held->profile());
        id = first.id();
        BOOST_CHECK_THROW(files::Record(f.temp.config(), f.held->profile(), id), files::Failure);
        struct stat s{};
        BOOST_REQUIRE_EQUAL(stat(f.record(id).c_str(), &s), 0);
        BOOST_CHECK_EQUAL(s.st_mode & 0777, 0600);
    }
    BOOST_REQUIRE_EQUAL(chmod(f.record(id).c_str(), 0644), 0);
    BOOST_CHECK_THROW(files::Record(f.temp.config(), f.held->profile(), id), files::Failure);
    for (unsigned n = 1; n < 1024; ++n) {
        std::ofstream file(std::filesystem::path(f.temp.config()) / "transfers" / (std::to_string(n) + ".json"));
        file << "{}";
    }
    auto source = f.temp.path / "source";
    {
        std::ofstream o(source);
        o << "abc";
    }
    auto result = f.client().upload(source.string(), SpaceId::parse(sid).value(), NodeId::parse(nid).value(), "file");
    BOOST_REQUIRE(!result);
    BOOST_CHECK_EQUAL(result.error().code, "TRANSFER_LIMIT_REACHED");
    BOOST_CHECK_EQUAL(f.http.calls, 0);
}
