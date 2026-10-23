#pragma once
#include <boost/test/unit_test.hpp>
#include <tests/integration/InitializedInstance.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/application/drive/DriveService.h>
#include <anof/application/drive/CreateDirectory.h>
#include <anof/application/identity/IdentityService.h>
#include <anof/application/transfer/UploadService.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/crypto/SodiumPasswordHasher.h>
#include <unistd.h>
namespace anof::testing::drive {
inline RequestId request() { return RequestId::parse("12345678-1234-4234-8234-123456789abc").value(); }
template<class T> T take(Result<T> result) { BOOST_REQUIRE(result); return std::move(result).value(); }
struct Fixture {
    std::filesystem::path root;
    runtime::MetadataWorkerResources resources;
    std::optional<RequestContext> admin;
    std::optional<Space> personal, common;
    Fixture() {
        char pattern[] = "/tmp/anof-drive-service-XXXXXX";
        auto* path = mkdtemp(pattern); BOOST_REQUIRE(path); root = path;
        BOOST_REQUIRE(initializeTestInstance(root));
        resources = take(runtime::makeInstanceMetadataResources(root / ".anof/metadata.sqlite3"));
        admin = login("administrator");
        auto read = take(store().beginDriveRead());
        auto actor = take(read->authorization().requireActiveSession(admin->credential, resources.clock->now()));
        personal = take(read->spaces().findPrivate(actor.userId));
        for (const auto& s : take(read->spaces().listAccessible(actor.userId))) if (s.kind == SpaceKind::Public) common = s;
        BOOST_REQUIRE(personal && common);
    }
    ~Fixture() { resources = {}; std::error_code ec; std::filesystem::remove_all(root, ec); }
    DriveMetadataStore& store() { auto* s = dynamic_cast<DriveMetadataStore*>(resources.metadata.get()); BOOST_REQUIRE(s); return *s; }
    DriveService service() { return {store(), *resources.clock, *resources.nodeIds, *resources.driveIds}; }
    IdentityService identity() { return {*resources.metadata, *resources.clock, *resources.identity->ids, *resources.identity->issuer, *resources.identity->invites}; }
    UploadService uploads() { return {store(), *resources.clock, *resources.transferIds, *resources.nodeIds}; }
    RequestContext login(const std::string& name) {
        auto s = identity();
        LoginCommand command{LoginName::parse(name).value(), PasswordInput::parse("fixture password only").value(), CredentialMode::Bearer, AuthenticationSource::ipv4({127,0,0,1})};
        auto snapshot = take(s.prepareLogin(command.login)); BOOST_REQUIRE(snapshot);
        auto result = take(s.completeLogin(request(), command, snapshot, true));
        auto codec = take(infrastructure::crypto::makeSodiumSessionTokenCodec());
        auto digest = take(codec->digestBearer(result.accessToken.view()));
        return {{digest, CredentialMode::Bearer}, request()};
    }
    RequestContext member(const std::string& name) {
        auto s = identity();
        auto invite = take(s.execute(*admin, CreateInviteCommand{InviteUseLimit::from(1).value(), InviteLifetimeSeconds::from(3600).value()}));
        RegisterCommand c{LoginName::parse(name).value(), DisplayName::parse(name).value(), PasswordInput::parse("fixture password only").value(), InviteTokenText::parse(invite.token.view()).value(), AuthenticationSource::ipv4({127,0,0,1})};
        auto hasher = take(infrastructure::crypto::makeSodiumPasswordHasher(runtime::instancePasswordPolicy()));
        auto hash = take(hasher->hash(c.password));
        take(s.registerUser(request(), c, std::move(hash)));
        return login(name);
    }
    NodeDetails node(NodeId id) { auto r = take(store().beginDriveRead()); auto n = take(r->nodes().find(id)); BOOST_REQUIRE(n); return *n; }
    NodeDetails mkdir(const RequestContext& ctx, Space space, NodeId parent, std::string name) {
        CreateDirectory s(*resources.metadata, *resources.clock, *resources.nodeIds);
        auto result = take(s.execute(ctx, {space.id, parent, std::move(name)}));
        return node(result.directory.id);
    }
    Upload createUpload(NodeId parent, std::string name, std::int64_t size = 0) {
        auto client = OperationId::parse(take(resources.transferIds->uploadId()).toString()).value();
        auto created = take(uploads().create(*admin, {client, {personal->id, {parent, std::move(name)}, FileLength::from(size).value(), {}}}, ByteCount::from(1048576).value(), {}));
        return take(uploads().opened(created.upload.id, created.upload.generation));
    }
    Upload publish(Upload u) {
        if (u.original.length.value()) {
            auto patch = take(uploads().preparePatch(*admin, {u.id, {}, ByteCount::from(u.original.length.value()).value()}));
            u = take(uploads().commitPatch(*admin, patch));
        }
        u = take(uploads().acceptComplete(*admin, {u.id, {}}));
        u = take(uploads().recordVerification(u.id, u.generation, Sha256::parse(std::string(64,'a')).value(), request()));
        u = take(uploads().preparePublish(u.id, u.generation));
        return take(uploads().commitPublish(u.id, u.generation, request()));
    }
    void reopen() { resources = {}; resources = take(runtime::makeInstanceMetadataResources(root / ".anof/metadata.sqlite3")); }
};
} // namespace anof::testing::drive
