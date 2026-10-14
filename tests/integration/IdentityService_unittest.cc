#define BOOST_TEST_MODULE IdentityServiceIntegration
#include <boost/test/unit_test.hpp>
#include <tests/integration/InitializedInstance.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/application/identity/IdentityService.h>
#include <anof/application/drive/CreateDirectory.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <unistd.h>
#include <algorithm>
using namespace anof;
using namespace anof::runtime;
namespace {
RequestId request() { return RequestId::parse("12345678-1234-4234-8234-123456789abc").value(); }
AuthenticationSource source() { return AuthenticationSource::ipv4({127,0,0,1}); }
LoginCommand loginCommand(std::string name = "administrator") {
    return {LoginName::parse(name).value(), PasswordInput::parse("fixture password only").value(), CredentialMode::Bearer, source()};
}
// 模拟真实提交成功后确认丢失；证明应用不会返回 token，也不自动重试。
struct UnknownTransaction final : MetadataTransaction {
    std::unique_ptr<MetadataTransaction> inner;
    explicit UnknownTransaction(std::unique_ptr<MetadataTransaction> value) : inner(std::move(value)) {}
    AuthorizationReader& authorization() override { return inner->authorization(); }
    UserRepository& users() override { return inner->users(); }
    SessionRepository& sessions() override { return inner->sessions(); }
    InviteRepository& invites() override { return inner->invites(); }
    SpaceRepository& spaces() override { return inner->spaces(); }
    NodeRepository& nodes() override { return inner->nodes(); }
    QuotaRepository& quotas() override { return inner->quotas(); }
    SettingsRepository& settings() override { return inner->settings(); }
    AuditRepository& directoryAudit() override { return inner->directoryAudit(); }
    IdentityAuditRepository& identityAudit() override { return inner->identityAudit(); }
    Result<void> commit() override {
        auto result = inner->commit();
        return result ? Result<void>{ErrorCode::CommitOutcomeUnknown} : result;
    }
};
struct UnknownStore final : MetadataStore {
    MetadataStore& inner;
    explicit UnknownStore(MetadataStore& value) : inner(value) {}
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override { return inner.beginRead(); }
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override {
        auto result = inner.beginWrite(); if (!result) return result.error();
        return std::unique_ptr<MetadataTransaction>(new UnknownTransaction(std::move(result).value()));
    }
};
struct Fixture {
    std::filesystem::path root;
    std::optional<MetadataWorkerResources> resources;
    std::optional<PasswordWorkerResources> password;
    std::unique_ptr<IdentityService> service;
    Fixture() {
        char pattern[] = "/tmp/anof-identity-service-XXXXXX";
        auto* path = ::mkdtemp(pattern); BOOST_REQUIRE(path); root = path;
        BOOST_REQUIRE(anof::testing::initializeTestInstance(root));
        auto opened = makeInstanceMetadataResources(root / ".anof/metadata.sqlite3"); BOOST_REQUIRE(opened);
        resources.emplace(std::move(opened).value());
        auto crypto = makeInstancePasswordResources(); BOOST_REQUIRE(crypto); password.emplace(std::move(crypto).value());
        service = std::make_unique<IdentityService>(*resources->metadata, *resources->clock, *resources->identity->ids,
                                                  *resources->identity->issuer, *resources->identity->invites);
    }
    ~Fixture() { service.reset(); resources.reset(); password.reset(); std::error_code ec; std::filesystem::remove_all(root, ec); }
    LoginResult login(std::string name = "administrator") {
        auto command = loginCommand(name);
        auto snapshot = service->prepareLogin(command.login); BOOST_REQUIRE(snapshot && snapshot.value());
        auto verified = password->hasher->verify(command.password, snapshot.value()->encodedHash); BOOST_REQUIRE(verified && verified.value());
        auto result = service->completeLogin(request(), command, snapshot.value(), verified.value()); BOOST_REQUIRE(result);
        return std::move(result).value();
    }
    RequestContext context(const LoginResult& login) {
        auto codec = infrastructure::crypto::makeSodiumSessionTokenCodec(); BOOST_REQUIRE(codec);
        auto digest = codec.value()->digestBearer(login.accessToken.view()); BOOST_REQUIRE(digest);
        return {{digest.value(), CredentialMode::Bearer}, request()};
    }
    CreatedInvite invite(const RequestContext& admin, int uses = 3) {
        auto result = service->execute(admin, CreateInviteCommand{InviteUseLimit::from(uses).value(), InviteLifetimeSeconds::from(3600).value()});
        BOOST_REQUIRE(result); return std::move(result).value();
    }
    Result<UserProfile> registerUser(std::string name, const CreatedInvite& invite) {
        RegisterCommand command{LoginName::parse(name).value(), DisplayName::parse(name).value(),
            PasswordInput::parse("fixture password only").value(), InviteTokenText::parse(invite.token.view()).value(), source()};
        auto hash = password->hasher->hash(command.password); BOOST_REQUIRE(hash);
        return service->registerUser(request(), command, std::move(hash).value());
    }
};
}
BOOST_AUTO_TEST_CASE(registration_visibility_paging_and_logout) {
    Fixture f;
    auto admin = f.login(); auto adminContext = f.context(admin);
    auto invite = f.invite(adminContext);
    auto member = f.registerUser("member_one", invite); BOOST_REQUIRE(member);
    BOOST_CHECK(member.value().role == UserRole::Member);
    auto login = f.login("member_one"); auto context = f.context(login);
    auto me = f.service->execute(context, GetMeCommand{}); BOOST_REQUIRE(me); BOOST_CHECK(me.value().id == member.value().id);
    auto spaces = f.service->execute(context, ListSpacesCommand{}); BOOST_REQUIRE(spaces); BOOST_REQUIRE_EQUAL(spaces.value().size(), 2);
    auto own = spaces.value().front();
    for (const auto& space : spaces.value()) {
        if (space.space.kind == SpaceKind::Private) own = space;
        else { BOOST_REQUIRE(space.myContribution); BOOST_CHECK_EQUAL(space.myContribution->state.used.nodes.value(), 0); }
    }
    BOOST_CHECK_EQUAL(own.quota.state.used.nodes.value(), 1);
    auto hidden = f.service->execute(adminContext, GetNodeCommand{own.space.rootNodeId});
    BOOST_REQUIRE(!hidden); BOOST_CHECK(hidden.error().code == ErrorCode::NotFound);
    auto denied = f.service->execute(context, ListInvitesCommand{PageSize::from(10).value(), {}});
    BOOST_REQUIRE(!denied); BOOST_CHECK(denied.error().code == ErrorCode::Forbidden);
    CreateDirectory mkdir(*f.resources->metadata, *f.resources->clock, *f.resources->nodeIds);
    BOOST_REQUIRE(mkdir.execute(context, {own.space.id, own.space.rootNodeId, "alpha"}));
    BOOST_REQUIRE(mkdir.execute(context, {own.space.id, own.space.rootNodeId, "beta"}));
    auto page = f.service->execute(context, ListChildrenCommand{own.space.rootNodeId, PageSize::from(1).value(), {}});
    BOOST_REQUIRE(page); BOOST_REQUIRE_EQUAL(page.value().items.size(), 1); BOOST_REQUIRE(page.value().next);
    auto next = f.service->execute(context, ListChildrenCommand{own.space.rootNodeId, PageSize::from(1).value(), page.value().next});
    BOOST_REQUIRE(next); BOOST_REQUIRE_EQUAL(next.value().items.size(), 1);
    BOOST_CHECK(page.value().items[0].metadata.id != next.value().items[0].metadata.id);
    page.value().next->userId = admin.user.id;
    auto wrongBinding = f.service->execute(context, ListChildrenCommand{own.space.rootNodeId, PageSize::from(1).value(), page.value().next});
    BOOST_REQUIRE(!wrongBinding); BOOST_CHECK(wrongBinding.error().code == ErrorCode::InvalidArgument);
    BOOST_REQUIRE(f.service->execute(context, LogoutCommand{}));
    BOOST_CHECK(!f.service->execute(context, GetMeCommand{}));
    BOOST_CHECK(!f.service->execute(context, LogoutCommand{}));
}
BOOST_AUTO_TEST_CASE(duplicate_registration_rolls_back_invite_consumption_and_revoke_is_effective) {
    Fixture f; auto login = f.login(); auto context = f.context(login); auto invite = f.invite(context, 1);
    auto duplicate = f.registerUser("administrator", invite);
    BOOST_REQUIRE(!duplicate); BOOST_CHECK(duplicate.error().code == ErrorCode::LoginConflict);
    BOOST_REQUIRE(f.registerUser("member_two", invite));
    auto exhausted = f.registerUser("member_three", invite);
    BOOST_REQUIRE(!exhausted); BOOST_CHECK(exhausted.error().code == ErrorCode::InviteUnavailable);
    auto other = f.invite(context);
    BOOST_REQUIRE(f.service->execute(context, RevokeInviteCommand{other.invite.id}));
    BOOST_REQUIRE(f.service->execute(context, RevokeInviteCommand{other.invite.id}));
    auto revoked = f.registerUser("member_four", other); BOOST_REQUIRE(!revoked);
    BOOST_CHECK(revoked.error().code == ErrorCode::InviteUnavailable);
    auto list = f.service->execute(context, ListInvitesCommand{PageSize::from(1).value(), {}});
    BOOST_REQUIRE(list && list.value().next);
    auto second = f.service->execute(context, ListInvitesCommand{PageSize::from(1).value(), list.value().next});
    BOOST_REQUIRE(second); BOOST_CHECK_EQUAL(second.value().items.size(), 1);
}
BOOST_AUTO_TEST_CASE(password_snapshot_rechecked_and_session_cap_enforced) {
    Fixture f; auto command = loginCommand();
    auto snapshot = f.service->prepareLogin(command.login); BOOST_REQUIRE(snapshot && snapshot.value());
    // 读快照已经关闭，允许独立写事务；修改 revision 后旧的密码结果不能签发 Session。
    auto tx = f.resources->metadata->beginWrite(); BOOST_REQUIRE(tx);
    auto changed = tx.value()->users().setDisplayName(snapshot.value()->userId, snapshot.value()->userRevision,
        DisplayName::parse("Changed administrator").value(), f.resources->clock->now()); BOOST_REQUIRE(changed);
    BOOST_REQUIRE(tx.value()->commit()); tx.value().reset();
    auto stale = f.service->completeLogin(request(), command, snapshot.value(), true);
    BOOST_REQUIRE(!stale); BOOST_CHECK(stale.error().code == ErrorCode::Unauthenticated);
    snapshot = f.service->prepareLogin(command.login); BOOST_REQUIRE(snapshot);
    for (int i = 0; i < 32; ++i) BOOST_REQUIRE(f.service->completeLogin(request(), command, snapshot.value(), true));
    auto capped = f.service->completeLogin(request(), command, snapshot.value(), true);
    BOOST_REQUIRE(!capped); BOOST_CHECK(capped.error().code == ErrorCode::SessionLimitReached);
    BOOST_REQUIRE(f.service->cleanupSessions());
    auto absent = f.service->prepareLogin(LoginName::parse("missing_user").value()); BOOST_REQUIRE(absent && !absent.value());
    auto unknown = f.service->completeLogin(request(), command, absent.value(), true);
    BOOST_REQUIRE(!unknown); BOOST_CHECK(unknown.error().code == ErrorCode::Unauthenticated);
}

BOOST_AUTO_TEST_CASE(uncertain_commit_returns_no_secrets_and_does_not_retry) {
    Fixture f; auto login = f.login(); auto context = f.context(login);
    UnknownStore store(*f.resources->metadata);
    IdentityService service(store, *f.resources->clock, *f.resources->identity->ids,
                            *f.resources->identity->issuer, *f.resources->identity->invites);
    auto cmd = loginCommand(); auto snapshot = service.prepareLogin(cmd.login); BOOST_REQUIRE(snapshot);
    auto issued = service.completeLogin(request(), cmd, snapshot.value(), true);
    BOOST_REQUIRE(!issued); BOOST_CHECK(issued.error().code == ErrorCode::CommitOutcomeUnknown);
    auto invitation = service.execute(context, CreateInviteCommand{InviteUseLimit::from(1).value(), InviteLifetimeSeconds::from(3600).value()});
    BOOST_REQUIRE(!invitation); BOOST_CHECK(invitation.error().code == ErrorCode::CommitOutcomeUnknown);
    auto read = f.resources->metadata->beginRead(); BOOST_REQUIRE(read);
    auto sessions = read.value()->sessions().countActive(login.user.id, f.resources->clock->now());
    BOOST_REQUIRE(sessions); BOOST_CHECK_EQUAL(sessions.value().value(), 2);
    auto invites = read.value()->invites().list(PageSize::from(200).value(), {});
    BOOST_REQUIRE(invites); BOOST_CHECK_EQUAL(invites.value().items.size(), 1);
}
BOOST_AUTO_TEST_CASE(password_change_and_disable_invalidate_verification_snapshot) {
    Fixture f; auto login = f.login(); auto context = f.context(login); auto invite = f.invite(context);
    auto member = f.registerUser("mutable_member", invite); BOOST_REQUIRE(member);
    auto command = loginCommand("mutable_member");
    auto snapshot = f.service->prepareLogin(command.login); BOOST_REQUIRE(snapshot && snapshot.value());
    auto hash = f.password->hasher->hash(command.password); BOOST_REQUIRE(hash);
    {
        auto tx = f.resources->metadata->beginWrite(); BOOST_REQUIRE(tx);
        auto changed = tx.value()->users().replacePassword(member.value().id,
            {snapshot.value()->passwordVersion, snapshot.value()->authEpoch, snapshot.value()->userRevision},
            hash.value(), f.resources->clock->now()); BOOST_REQUIRE(changed); BOOST_REQUIRE(tx.value()->commit());
    }
    auto stale = f.service->completeLogin(request(), command, snapshot.value(), true);
    BOOST_REQUIRE(!stale); BOOST_CHECK(stale.error().code == ErrorCode::Unauthenticated);
    snapshot = f.service->prepareLogin(command.login); BOOST_REQUIRE(snapshot && snapshot.value());
    {
        auto tx = f.resources->metadata->beginWrite(); BOOST_REQUIRE(tx);
        BOOST_REQUIRE(tx.value()->users().setEnabled(member.value().id, snapshot.value()->userRevision, false, f.resources->clock->now()));
        BOOST_REQUIRE(tx.value()->commit());
    }
    auto disabled = f.service->completeLogin(request(), command, snapshot.value(), true);
    BOOST_REQUIRE(!disabled); BOOST_CHECK(disabled.error().code == ErrorCode::Unauthenticated);
}

BOOST_AUTO_TEST_CASE(child_cursor_observes_between_page_commits_and_rechecks_session) {
    Fixture f;
    auto login = f.login();
    const auto context = f.context(login);
    auto spaces = f.service->execute(context, ListSpacesCommand{});
    BOOST_REQUIRE(spaces);
    const auto found = std::find_if(spaces.value().begin(), spaces.value().end(),
                                    [](const auto& value) { return value.space.kind == SpaceKind::Private; });
    BOOST_REQUIRE(found != spaces.value().end());
    const auto own = *found;
    // 独立连接在两次读取之间提交，续页不持有上一页的快照。
    auto other = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3");
    BOOST_REQUIRE(other);
    CreateDirectory mkdir(*other.value().metadata, *other.value().clock, *other.value().nodeIds);
    BOOST_REQUIRE(mkdir.execute(context, {own.space.id, own.space.rootNodeId, "bravo"}));
    BOOST_REQUIRE(mkdir.execute(context, {own.space.id, own.space.rootNodeId, "delta"}));
    auto first = f.service->execute(context, ListChildrenCommand{own.space.rootNodeId, PageSize::from(1).value(), {}});
    BOOST_REQUIRE(first && first.value().next);
    BOOST_REQUIRE_EQUAL(first.value().items.size(), 1);
    BOOST_CHECK_EQUAL(first.value().items.front().metadata.name, "bravo");
    BOOST_REQUIRE(mkdir.execute(context, {own.space.id, own.space.rootNodeId, "alpha"}));
    BOOST_REQUIRE(mkdir.execute(context, {own.space.id, own.space.rootNodeId, "charlie"}));
    auto next = f.service->execute(context, ListChildrenCommand{own.space.rootNodeId, PageSize::from(1).value(), first.value().next});
    BOOST_REQUIRE(next && next.value().next);
    BOOST_REQUIRE_EQUAL(next.value().items.size(), 1);
    BOOST_CHECK_EQUAL(next.value().items.front().metadata.name, "charlie");
    auto last = f.service->execute(context, ListChildrenCommand{own.space.rootNodeId, PageSize::from(1).value(), next.value().next});
    BOOST_REQUIRE(last);
    BOOST_REQUIRE_EQUAL(last.value().items.size(), 1);
    BOOST_CHECK_EQUAL(last.value().items.front().metadata.name, "delta");
    BOOST_CHECK(!last.value().next);
    BOOST_REQUIRE(f.service->execute(context, LogoutCommand{}));
    auto revoked = f.service->execute(context, ListChildrenCommand{own.space.rootNodeId, PageSize::from(1).value(), first.value().next});
    BOOST_REQUIRE(!revoked);
    BOOST_CHECK(revoked.error().code == ErrorCode::Unauthenticated);
}
