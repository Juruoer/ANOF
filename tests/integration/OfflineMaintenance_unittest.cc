#define BOOST_TEST_MODULE OfflineMaintenance
#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/infrastructure/crypto/SodiumMaintenanceDigest.h>
#include <anof/infrastructure/crypto/SodiumPasswordHasher.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/runtime/InstanceMaintenance.h>
#include <apps/anof/MaintenancePlanJson.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <tests/integration/ClientProcess.h>
#include <tests/integration/DriveFixture.h>
#include <tests/integration/InitializedInstance.h>
using namespace anof;
using namespace anof::testing;
using namespace anof::testing::cli;
namespace {
struct Fixture {
    Temp temp;
    std::filesystem::path root = temp.path / "source";
    Fixture() {
        BOOST_REQUIRE_EQUAL(::mkdir(root.c_str(), 0700), 0);
        BOOST_REQUIRE(initializeTestInstance(root));
    }
    std::vector<std::string> cmd(std::vector<std::string> args) {
        std::vector<std::string> out{ "--json" };
        out.insert(out.end(), args.begin(), args.end());
        out.insert(out.end(), { "--root", root.string() });
        return out;
    }
    Json run(std::vector<std::string> args, int code = 0) {
        Process p(cmd(std::move(args)));
        p.finish(code);
        if (code) {
            BOOST_REQUIRE_MESSAGE(!p.err.empty(), p.out);
            return Json::parse(p.err);
        }
        BOOST_REQUIRE_MESSAGE(!p.out.empty(), p.err);
        return Json::parse(p.out);
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(config_uses_existing_safe_configuration_and_lock) {
    Fixture f;
    auto config = f.run({ "config", "show" });
    BOOST_CHECK_EQUAL(config.at("source"), "instance-file");
    BOOST_CHECK(config.at("development_http").get<bool>());
    BOOST_CHECK_EQUAL(config.at("audit_retention_days"), 90);
    auto valid = f.run({ "config", "validate" });
    BOOST_CHECK(valid.at("valid").get<bool>());
    BOOST_CHECK(!valid.at("ready").get<bool>());
    auto lock = infrastructure::system::makeLinuxInstanceLockProvider();
    BOOST_REQUIRE(lock);
    auto lease = lock.value()->acquire(f.root);
    BOOST_REQUIRE(lease);
    auto busy = f.run({ "config", "show" }, 3);
    BOOST_CHECK_EQUAL(busy.at("code"), "BUSY");
}
BOOST_AUTO_TEST_CASE(fsck_jsonl_and_external_plan_are_strict_and_non_overwriting) {
    Fixture f;
    auto plan = f.temp.path / "plan.json", report = f.temp.path / "report.jsonl";
    Process inspect(f.cmd({ "fsck", "--repair", "--plan-out", plan.string(), "--report", report.string() }));
    inspect.finish(0);
    std::istringstream lines(inspect.out);
    std::string line;
    BOOST_REQUIRE(std::getline(lines, line));
    BOOST_CHECK_EQUAL(Json::parse(line).at("type"), "header");
    BOOST_REQUIRE(std::getline(lines, line));
    BOOST_CHECK_EQUAL(Json::parse(line).at("status"), "healthy");
    auto data = jsonFile(plan);
    auto parsed = app::parseMaintenancePlan(data.dump());
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(!app::parseMaintenancePlan("{\"plan_format\":1," + data.dump().substr(1)));
    data["extra"] = true;
    BOOST_CHECK(!app::parseMaintenancePlan(data.dump()));
    data.erase("extra");
    data["inspection"]["info"] = "00";
    BOOST_CHECK(!app::parseMaintenancePlan(data.dump()));
    Process repeat(f.cmd({ "fsck", "--repair", "--plan-out", plan.string() }));
    repeat.finish(3);
    Process noConfirm(f.cmd({ "fsck", "--apply-plan", plan.string(), "--backup-dir", (f.temp.path / "original").string() }));
    noConfirm.finish(2);
    BOOST_CHECK(!std::filesystem::exists(f.temp.path / "original"));
    struct stat st{};
    BOOST_REQUIRE_EQUAL(::stat(report.c_str(), &st), 0);
    BOOST_CHECK_EQUAL(st.st_mode & 0777, 0600);
}
BOOST_AUTO_TEST_CASE(backup_restore_new_directory_and_full_fsck) {
    Fixture f;
    const auto backup = f.temp.path / "backup", restored = f.temp.path / "restored";
    auto result = f.run({ "backup", "--output", backup.string() });
    BOOST_CHECK_EQUAL(result.at("outcome"), "confirmed");
    BOOST_CHECK(std::filesystem::exists(backup / "COMPLETE"));
    Process restore({ "--json", "restore", "--backup", backup.string(), "--root", restored.string(), "--config", (f.root / ".anof/config.toml").string() });
    restore.finish(0);
    BOOST_REQUIRE_MESSAGE(std::filesystem::exists(restored / ".anof/instance.json"), restore.err);
    Process check({ "--json", "fsck", "--full", "--root", restored.string() });
    check.finish(0);
    BOOST_CHECK(check.out.find("\"status\":\"healthy\"") != std::string::npos);
    Process duplicate({ "--json", "restore", "--backup", backup.string(), "--root", restored.string(), "--config", (f.root / ".anof/config.toml").string() });
    duplicate.finish(3);
    auto db = infrastructure::sqlite::openSqliteDatabase(restored / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(db);
    auto statement = infrastructure::sqlite::SqliteStatement::prepare(*db.value(), "SELECT (SELECT count(*) FROM sessions)+(SELECT count(*) FROM uploads)+(SELECT count(*) FROM invites)");
    BOOST_REQUIRE(statement);
    BOOST_REQUIRE(statement.value()->step().value());
    BOOST_CHECK_EQUAL(statement.value()->integer(0).value(), 0);
}
BOOST_AUTO_TEST_CASE(backup_rejects_live_source_and_preserves_existing_destination) {
    Fixture f;
    const auto backup = f.temp.path / "backup";
    {
        Process server({ "serve", "--root", f.root.string() });
        BOOST_REQUIRE(server.port() > 0);
        auto busy = f.run({ "backup", "--output", backup.string() }, 3);
        BOOST_CHECK_EQUAL(busy.at("code"), "BUSY");
        BOOST_CHECK(!std::filesystem::exists(backup));
        server.stop();
    }
    f.run({ "backup", "--output", backup.string() });
    const auto manifest = jsonFile(backup / "manifest.json");
    f.run({ "backup", "--output", backup.string() }, 3);
    BOOST_CHECK(jsonFile(backup / "manifest.json") == manifest);
    const auto restored = f.temp.path / "restored";
    Process restore({ "--json", "restore", "--backup", backup.string(), "--root", restored.string(), "--config", (f.root / ".anof/config.toml").string() });
    restore.finish(0);
}

BOOST_AUTO_TEST_CASE(restore_rejects_invalid_marker_and_extra_files_without_publishing) {
    Fixture f;
    const auto backup = f.temp.path / "backup";
    f.run({ "backup", "--output", backup.string() });
    std::ifstream markerInput(backup / "COMPLETE");
    const std::string marker((std::istreambuf_iterator<char>(markerInput)), {});
    const auto rejected = [&](const char* name) {
        const auto target = f.temp.path / name;
        Process restore({ "--json", "restore", "--backup", backup.string(), "--root", target.string(), "--config", (f.root / ".anof/config.toml").string() });
        restore.finish(4);
        const auto error = Json::parse(restore.err);
        BOOST_CHECK_EQUAL(error.at("code"), "INTEGRITY_ERROR");
        BOOST_CHECK_EQUAL(error.at("outcome"), "not_started");
        BOOST_CHECK(!std::filesystem::exists(target));
    };
    file(backup / "COMPLETE", std::string(64, '0') + "\n");
    rejected("bad-marker");
    file(backup / "COMPLETE", marker);
    file(backup / "unexpected", "unlisted content");
    BOOST_REQUIRE_EQUAL(::chmod((backup / "unexpected").c_str(), 0600), 0);
    rejected("extra-file");
    BOOST_CHECK(std::filesystem::exists(backup / "unexpected"));
    Process sourceCheck(f.cmd({ "fsck", "--full" }));
    sourceCheck.finish(0);
}

BOOST_AUTO_TEST_CASE(administrator_reset_is_terminal_only_and_audited) {
    Fixture f;
    Process rejected(f.cmd({ "admin", "reset-password", "--password", "must-not-log" }));
    rejected.finish(2);
    BOOST_CHECK(rejected.err.find("must-not-log") == std::string::npos);
    Process reset(f.cmd({ "admin", "reset-password" }), true);
    reset.input("Administrator password: ", "new maintenance password");
    reset.input("Confirm password: ", "new maintenance password");
    reset.finish(0);
    BOOST_CHECK(reset.tty.find("new maintenance password") == std::string::npos);
    BOOST_CHECK(reset.out.find("new maintenance password") == std::string::npos);
    auto db = infrastructure::sqlite::openSqliteDatabase(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(db);
    auto q = infrastructure::sqlite::SqliteStatement::prepare(*db.value(), "SELECT count(*) FROM identity_audit_events WHERE action='administrator_password_reset' AND actor_id IS NULL");
    BOOST_REQUIRE(q);
    BOOST_REQUIRE(q.value()->step().value());
    BOOST_CHECK_EQUAL(q.value()->integer(0).value(), 1);
}
BOOST_AUTO_TEST_CASE(orphan_plan_apply_reentry_and_explicit_backup_exclusion) {
    Fixture f;
    auto orphan = f.root / ".anof/uploads/12345678-1234-4234-8234-123456789abc.part";
    file(orphan, "preserve orphan bytes");
    BOOST_REQUIRE_EQUAL(::chmod(orphan.c_str(), 0600), 0);
    const auto plan = f.temp.path / "repair.json", original = f.temp.path / "original";
    Process inspect(f.cmd({ "fsck", "--full", "--repair", "--plan-out", plan.string() }));
    inspect.finish(4);
    BOOST_CHECK(std::filesystem::exists(orphan));
    for (int i = 0; i < 2; ++i) {
        Process apply(f.cmd({ "fsck", "--apply-plan", plan.string(), "--confirm-repair", "--backup-dir", original.string() }));
        apply.finish(4);
        BOOST_CHECK_MESSAGE(apply.err.empty(), apply.err);
        BOOST_CHECK(apply.out.find("QUARANTINE_PRESENT") != std::string::npos);
    }
    BOOST_CHECK(!std::filesystem::exists(orphan));
    BOOST_CHECK(std::filesystem::exists(original / "COMPLETE"));
    auto refused = f.run({ "backup", "--output", (f.temp.path / "refused-backup").string() }, 4);
    BOOST_CHECK_EQUAL(refused.at("code"), "QUARANTINE_PRESENT");
    auto backed = f.run({ "backup", "--exclude-quarantine", "--output", (f.temp.path / "excluded-backup").string() });
    BOOST_CHECK_EQUAL(backed.at("exclusions").at("quarantine_objects"), "1");
}

BOOST_AUTO_TEST_CASE(real_serve_logs_hide_targets_credentials_and_library_messages) {
    Fixture f;
    Process server({ "serve", "--root", f.root.string() });
    auto port = server.port();
    int socket = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    BOOST_REQUIRE(socket >= 0);
    timeval deadline{ 2, 0 };
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof deadline);
    ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof deadline);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOST_REQUIRE_EQUAL(::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
    const std::string request = "GET /never-log-this-path?token=never-log-this-query HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer never-log-this-bearer\r\nCookie: secret=never-log-this-cookie\r\nConnection: close\r\n\r\n";
    BOOST_REQUIRE_EQUAL(::send(socket, request.data(), request.size(), MSG_NOSIGNAL), static_cast<ssize_t>(request.size()));
    char bytes[4096];
    while (::recv(socket, bytes, sizeof bytes, 0) > 0) {
    }
    ::close(socket);
    server.stop();
    BOOST_CHECK(server.out.find("never-log-this") == std::string::npos);
    BOOST_CHECK(server.err.find("never-log-this") == std::string::npos);
    bool access = false;
    std::istringstream lines(server.out);
    std::string line;
    while (std::getline(lines, line)) {
        auto record = Json::parse(line);
        BOOST_CHECK(line.size() <= 4096);
        if (record.value("event", "") == "http_access") {
            access = true;
            BOOST_CHECK_EQUAL(record.at("route"), "unmatched");
            BOOST_CHECK(record.at("request_id").is_string());
        }
    }
    BOOST_CHECK(access);
}
BOOST_AUTO_TEST_CASE(reset_interrupt_restores_terminal_and_report_failure_preserves_confirmed_outcome) {
    Fixture f;
    Process interrupted(f.cmd({ "admin", "reset-password" }), true);
    interrupted.until([&] { return interrupted.tty.find("Administrator password: ") != std::string::npos; });
    BOOST_REQUIRE_EQUAL(::kill(interrupted.pid, SIGINT), 0);
    interrupted.finish(130);
    Process outputFailure(f.cmd({ "config", "validate" }), false, true);
    outputFailure.finish(5);
    auto error = Json::parse(outputFailure.err);
    BOOST_CHECK_EQUAL(error.at("outcome"), "confirmed");
    BOOST_CHECK_EQUAL(error.at("stage"), "report");
}

BOOST_AUTO_TEST_CASE(real_content_backup_preserves_shared_trash_and_clears_only_copy_credentials) {
    anof::testing::drive::Fixture drive;
    Temp artifacts;
    auto uploaded = drive.createUpload(drive.personal->rootNodeId, "content", 7);
    const std::string bytes = "payload";
    infrastructure::crypto::SodiumMaintenanceDigest digest;
    auto hash = digest.digest(bytes);
    BOOST_REQUIRE(hash);
    auto uploadService = drive.uploads();
    auto patch = anof::testing::drive::take(uploadService.preparePatch(*drive.admin, { uploaded.id, {}, ByteCount::from(7).value() }));
    uploaded = anof::testing::drive::take(uploadService.commitPatch(*drive.admin, patch));
    uploaded = anof::testing::drive::take(uploadService.acceptComplete(*drive.admin, { uploaded.id, {} }));
    uploaded = anof::testing::drive::take(uploadService.recordVerification(uploaded.id, uploaded.generation, hash.value(), anof::testing::drive::request()));
    uploaded = anof::testing::drive::take(uploadService.preparePublish(uploaded.id, uploaded.generation));
    uploaded = anof::testing::drive::take(uploadService.commitPublish(uploaded.id, uploaded.generation, anof::testing::drive::request()));
    auto object = drive.root / ".anof/objects" / uploaded.pendingBlobId->toString().substr(0, 2) / uploaded.pendingBlobId->toString();
    BOOST_REQUIRE_EQUAL(::mkdir(object.parent_path().c_str(), 0700), 0);
    file(object, bytes);
    BOOST_REQUIRE_EQUAL(::chmod(object.c_str(), 0600), 0);
    auto node = drive.node(*uploaded.resultNodeId);
    auto copy = anof::testing::drive::take(drive.service().execute(*drive.admin, CopyFileCommand{ node.metadata.id, NodeCondition{ node.metadata.id, node.metadata.revision }, drive.personal->id, drive.personal->rootNodeId, "copy" }));
    anof::testing::drive::take(drive.service().execute(*drive.admin, TrashNodeCommand{ copy.metadata.id, NodeCondition{ copy.metadata.id, copy.metadata.revision } }));
    auto active = drive.createUpload(drive.personal->rootNodeId, "unfinished", 3);
    auto part = drive.root / ".anof/uploads" / (active.id.toString() + ".part");
    file(part, "");
    BOOST_REQUIRE_EQUAL(::chmod(part.c_str(), 0600), 0);
    auto creatingClient = OperationId::parse(anof::testing::drive::take(drive.resources.transferIds->uploadId()).toString()).value();
    auto creating = drive.uploads().create(*drive.admin, { creatingClient, { drive.personal->id, { drive.personal->rootNodeId, "creating" }, FileLength::from(0).value(), {} } }, ByteCount::from(1048576).value(), {});
    BOOST_REQUIRE(creating);
    const auto creatingId = creating.value().upload.id;
    auto invite = drive.identity().execute(*drive.admin, CreateInviteCommand{ InviteUseLimit::from(1).value(), InviteLifetimeSeconds::from(3600).value() });
    BOOST_REQUIRE(invite);
    drive.resources = {};
    auto backup = artifacts.path / "backup", restored = artifacts.path / "restored";
    Process create({ "--json", "backup", "--root", drive.root.string(), "--output", backup.string() });
    create.finish(0);
    BOOST_REQUIRE_MESSAGE(!create.out.empty(), create.err);
    auto manifest = jsonFile(backup / "manifest.json");
    BOOST_CHECK_EQUAL(manifest.at("object_index").at("count"), "1");
    Process restore({ "--json", "restore", "--root", restored.string(), "--backup", backup.string(), "--config", (drive.root / ".anof/config.toml").string() });
    restore.finish(0);
    Process full({ "--json", "fsck", "--full", "--root", restored.string() });
    full.finish(0);
    drive.reopen();
    {
        auto read = anof::testing::drive::take(drive.store().beginDriveRead());
        BOOST_REQUIRE(read->authorization().requireActiveSession(drive.admin->credential, drive.resources.clock->now()));
    }
    BOOST_CHECK(anof::testing::drive::take(drive.uploads().read(active.id)).state == UploadState::Open);
    BOOST_CHECK(anof::testing::drive::take(drive.uploads().read(creatingId)).state == UploadState::Open);
    BOOST_CHECK(std::filesystem::exists(drive.root / ".anof/uploads" / (creatingId.toString() + ".part")));
    auto resources = runtime::makeInstanceMetadataResources(restored / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(resources);
    {
        auto read = resources.value().metadata->beginRead();
        BOOST_REQUIRE(read);
        BOOST_CHECK(!read.value()->authorization().requireActiveSession(drive.admin->credential, resources.value().clock->now()));
        read.value().reset();
        read = resources.value().metadata->beginRead();
        BOOST_REQUIRE(read);
        auto found = read.value()->nodes().find(node.metadata.id);
        BOOST_REQUIRE(found && found.value());
    }
    std::ifstream content(restored / object.lexically_relative(drive.root));
    std::string restoredBytes((std::istreambuf_iterator<char>(content)), {});
    BOOST_CHECK_EQUAL(restoredBytes, bytes);
    resources.value() = {};
    Process server({ "serve", "--root", restored.string() });
    auto port = server.port();
    anof::testing::cli::run(artifacts, "", { "profile", "add", "restored", "--server", "http://127.0.0.1:" + std::to_string(port), "--development-http" });
    anof::testing::cli::login(artifacts, "restored", "administrator", "fixture password only");
    anof::testing::cli::run(artifacts, "restored", { "stat", node.metadata.id.toString() });
    auto downloaded = artifacts.path / "downloaded";
    anof::testing::cli::run(artifacts, "restored", { "download", node.metadata.id.toString(), "--output", downloaded.string() });
    std::ifstream downloadedFile(downloaded);
    std::string downloadedBytes((std::istreambuf_iterator<char>(downloadedFile)), {});
    BOOST_CHECK_EQUAL(downloadedBytes, bytes);
    server.stop();
    BOOST_CHECK(server.out.find("fixture password only") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(local_reset_revokes_existing_session_and_replaces_hash) {
    anof::testing::drive::Fixture drive;
    LoginCommand command{ LoginName::parse("administrator").value(), PasswordInput::parse("fixture password only").value(), CredentialMode::Cookie, AuthenticationSource::ipv4({ 127, 0, 0, 1 }) };
    auto snapshot = drive.identity().prepareLogin(command.login);
    BOOST_REQUIRE(snapshot && snapshot.value());
    auto cookie = drive.identity().completeLogin(anof::testing::drive::request(), command, snapshot.value(), true);
    BOOST_REQUIRE(cookie);
    auto codec = infrastructure::crypto::makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    auto digest = codec.value()->digestBearer(cookie.value().accessToken.view());
    BOOST_REQUIRE(digest);
    const SessionCredential cookieCredential{ digest.value(), CredentialMode::Cookie };
    drive.resources = {};
    Process reset({ "--json", "admin", "reset-password", "--root", drive.root.string() }, true);
    reset.input("Administrator password: ", "replacement password");
    reset.input("Confirm password: ", "replacement password");
    reset.finish(0);
    drive.reopen();
    auto read = drive.store().beginRead();
    BOOST_REQUIRE(read);
    BOOST_CHECK(!read.value()->authorization().requireActiveSession(drive.admin->credential, drive.resources.clock->now()));
    read.value().reset();
    read = drive.store().beginRead();
    BOOST_REQUIRE(read);
    BOOST_CHECK(!read.value()->authorization().requireActiveSession(cookieCredential, drive.resources.clock->now()));
    read.value().reset();
    read = drive.store().beginRead();
    BOOST_REQUIRE(read);
    auto passwordSnapshot = read.value()->users().findPassword(LoginName::parse("administrator").value());
    BOOST_REQUIRE(passwordSnapshot && passwordSnapshot.value());
    auto hasher = infrastructure::crypto::makeSodiumPasswordHasher(runtime::instancePasswordPolicy());
    BOOST_REQUIRE(hasher);
    auto verified = hasher.value()->verify(PasswordInput::parse("replacement password").value(), passwordSnapshot.value()->encodedHash);
    BOOST_REQUIRE(verified);
    BOOST_CHECK(verified.value());
}

BOOST_AUTO_TEST_CASE(readonly_report_blocked_by_pipe_can_be_interrupted_without_hanging) {
    Fixture f;
    auto report = f.temp.path / "interrupted-report.jsonl";
    Process inspect(f.cmd({ "fsck", "--report", report.string() }), false, false, true);
    bool complete = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream stream(report);
        std::string text((std::istreambuf_iterator<char>(stream)), {});
        if (text.find("\"type\":\"summary\"") != std::string::npos) {
            complete = true;
            break;
        }
        pollfd unused{ -1, 0, 0 };
        ::poll(&unused, 1, 5);
    }
    BOOST_REQUIRE(complete);
    BOOST_REQUIRE_EQUAL(::kill(inspect.pid, SIGINT), 0);
    // 信号被worker观察并退出前，不消费已填满的stdout，保持确定的背压窗口。
    int status = 0;
    bool exited = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(inspect.pid, &status, WNOHANG) == inspect.pid) {
            inspect.status = status;
            exited = true;
            break;
        }
        pollfd unused{ -1, 0, 0 };
        ::poll(&unused, 1, 5);
    }
    BOOST_REQUIRE(exited);
    inspect.finish(130);
    auto error = Json::parse(inspect.err);
    BOOST_CHECK_EQUAL(error.at("code"), "CANCELLED");
}
