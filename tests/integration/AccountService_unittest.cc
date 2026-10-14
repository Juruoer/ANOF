#define BOOST_TEST_MODULE AccountServiceIntegration
#include <boost/test/unit_test.hpp>
#include <tests/integration/InitializedInstance.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/application/identity/IdentityService.h>
#include <anof/application/identity/AccountService.h>
#include <anof/application/drive/CreateDirectory.h>
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <unistd.h>
#include <functional>
using namespace anof;
using namespace anof::runtime;
namespace {
RequestId request() { return RequestId::parse("12345678-1234-4234-8234-123456789abc").value(); }
AuthenticationSource source() { return AuthenticationSource::ipv4({127,0,0,1}); }
LoginCommand loginCommand(std::string name = "administrator") {
    return {LoginName::parse(name).value(), PasswordInput::parse("fixture password only").value(), CredentialMode::Bearer, source()};
}
// 模拟真实提交成功后确认丢失；证明应用不会返回 token，也不自动重试。
struct UnknownTransaction : MetadataTransaction {
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
struct AuditControl { unsigned calls = 0; bool fail = false; std::function<void()> onAppend; };
struct AuditedTransaction final : UnknownTransaction, IdentityAuditRepository {
    AuditControl& control;
    AuditedTransaction(std::unique_ptr<MetadataTransaction> tx, AuditControl& value)
        : UnknownTransaction(std::move(tx)), control(value) {}
    IdentityAuditRepository& identityAudit() override { return *this; }
    Result<void> append(const IdentityEvent& event) override {
        ++control.calls;
        auto appended = inner->identityAudit().append(event);
        if (!appended) return appended.error();
        if (control.onAppend) control.onAppend();
        return control.fail ? Result<void>{ErrorCode::InternalError} : Result<void>{};
    }
    Result<void> commit() override { return inner->commit(); }
};
struct AuditedStore final : MetadataStore {
    MetadataStore& inner; AuditControl& control;
    AuditedStore(MetadataStore& store, AuditControl& value) : inner(store), control(value) {}
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override { return inner.beginRead(); }
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override {
        auto tx = inner.beginWrite(); if (!tx) return tx.error();
        return std::unique_ptr<MetadataTransaction>(new AuditedTransaction(std::move(tx).value(), control));
    }
};
struct TestClock : Clock {
    UtcTime time;
    explicit TestClock(UtcTime value) : time(value) {}
    UtcTime now() const override { return time; }
};
struct Fixture {
    std::filesystem::path root;
    std::optional<MetadataWorkerResources> resources;
    std::optional<PasswordWorkerResources> password;
    std::unique_ptr<IdentityService> service;
    std::unique_ptr<AccountService> accounts;
    TestClock* clock;
    Fixture() {
        char pattern[] = "/tmp/anof-account-service-XXXXXX";
        auto* path = ::mkdtemp(pattern); BOOST_REQUIRE(path); root = path;
        BOOST_REQUIRE(anof::testing::initializeTestInstance(root));
        auto opened = makeInstanceMetadataResources(root / ".anof/metadata.sqlite3"); BOOST_REQUIRE(opened);
        resources.emplace(std::move(opened).value());
        auto crypto = makeInstancePasswordResources(); BOOST_REQUIRE(crypto); password.emplace(std::move(crypto).value());
        auto testClock = std::make_unique<TestClock>(resources->clock->now());
        clock = testClock.get(); resources->clock = std::move(testClock);
        accounts = std::make_unique<AccountService>(*resources->metadata, *resources->clock);
        service = std::make_unique<IdentityService>(*resources->metadata, *resources->clock, *resources->identity->ids,
                                                  *resources->identity->issuer, *resources->identity->invites);
    }
    ~Fixture() { accounts.reset(); service.reset(); resources.reset(); password.reset(); std::error_code ec; std::filesystem::remove_all(root, ec); }
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
AccountCondition condition(const UserProfile& user) { return UserCondition{user.id, user.revision}; }
AccountCondition condition(const ManagedQuota& quota) { return QuotaCondition{quota.scope, quota.quota.revision}; }
template<class T> void errorIs(const Result<T>& result, ErrorCode code) {
    BOOST_REQUIRE(!result); BOOST_CHECK(result.error().code == code);
}
EncodedPasswordHash newHash(Fixture& f) {
    auto input = PasswordInput::parse("replacement password only").value();
    auto result = f.password->hasher->hash(input); BOOST_REQUIRE(result); return std::move(result).value();
}

}
BOOST_AUTO_TEST_CASE(account_authorization_paging_conditions_and_private_isolation) {
    Fixture f; auto admin = f.login(); auto ac = f.context(admin); auto invite = f.invite(ac);
    auto member = f.registerUser("member_one", invite); BOOST_REQUIRE(member);
    auto login = f.login("member_one"); auto mc = f.context(login);
    auto missing = UserId::parse("99999999-1111-4111-8111-111111111111").value();
    errorIs(f.accounts->execute(mc, GetUserCommand{missing}), ErrorCode::Forbidden);
    errorIs(f.accounts->execute(ac, GetUserCommand{missing}), ErrorCode::NotFound);
    auto page = f.accounts->execute(ac, ListUsersCommand{PageSize::from(1).value(), {}});
    BOOST_REQUIRE(page && page.value().next); BOOST_REQUIRE_EQUAL(page.value().items.size(), 1);
    auto next = f.accounts->execute(ac, ListUsersCommand{PageSize::from(1).value(), page.value().next});
    BOOST_REQUIRE(next); BOOST_CHECK(!next.value().next); BOOST_REQUIRE_EQUAL(next.value().items.size(), 1);
    BOOST_CHECK(page.value().items[0].id != next.value().items[0].id);
    page.value().next->userId = member.value().id;
    errorIs(f.accounts->execute(ac, ListUsersCommand{PageSize::from(1).value(), page.value().next}), ErrorCode::InvalidArgument);
    errorIs(f.accounts->execute(mc, ListUsersCommand{PageSize::from(1).value(), {}}), ErrorCode::Forbidden);
    auto detail = f.accounts->execute(ac, GetUserCommand{member.value().id}); BOOST_REQUIRE(detail);
    auto spaces = f.service->execute(mc, ListSpacesCommand{}); BOOST_REQUIRE(spaces);
    for (const auto& view : spaces.value()) if (view.space.kind == SpaceKind::Private) {
        BOOST_CHECK(detail.value().privateSpaceId == view.space.id);
        errorIs(f.service->execute(ac, GetNodeCommand{view.space.rootNodeId}), ErrorCode::NotFound);
        errorIs(f.accounts->execute(ac, GetQuotaCommand{PublicContributionQuota{view.space.id, member.value().id}}), ErrorCode::NotFound);
    }
    errorIs(f.accounts->execute(mc, UpdateDisplayNameCommand{condition(admin.user), DisplayName::parse("No").value()}), ErrorCode::PreconditionFailed);
    errorIs(f.accounts->execute(mc, UpdateDisplayNameCommand{QuotaCondition{SpaceQuota{detail.value().privateSpaceId}, Revision{}}, DisplayName::parse("No").value()}), ErrorCode::PreconditionFailed);
    auto renamed = f.accounts->execute(mc, UpdateDisplayNameCommand{condition(member.value()), DisplayName::parse("Renamed").value()});
    BOOST_REQUIRE(renamed); BOOST_CHECK_EQUAL(renamed.value().revision.value(), 2);
    BOOST_CHECK(renamed.value().authEpoch == member.value().authEpoch);
    BOOST_REQUIRE(f.service->execute(mc, GetMeCommand{}));
    auto same = f.accounts->execute(mc, UpdateDisplayNameCommand{condition(renamed.value()), renamed.value().displayName});
    BOOST_REQUIRE(same); BOOST_CHECK(same.value().revision == renamed.value().revision);
    BOOST_CHECK(same.value().updatedAt == renamed.value().updatedAt);
    errorIs(f.accounts->execute(mc, UpdateDisplayNameCommand{condition(member.value()), renamed.value().displayName}), ErrorCode::PreconditionFailed);
}
BOOST_AUTO_TEST_CASE(disable_reenable_revokes_all_and_protects_administrator) {
    Fixture f; auto admin = f.login(); auto ac = f.context(admin); auto invite = f.invite(ac);
    auto member = f.registerUser("member_one", invite); BOOST_REQUIRE(member);
    auto first = f.login("member_one"); auto second = f.login("member_one");
    errorIs(f.accounts->execute(ac, SetUserEnabledCommand{admin.user.id, condition(admin.user), false}), ErrorCode::Forbidden);
    errorIs(f.accounts->execute(f.context(first), SetUserEnabledCommand{member.value().id, condition(member.value()), false}), ErrorCode::Forbidden);
    auto disabled = f.accounts->execute(ac, SetUserEnabledCommand{member.value().id, condition(member.value()), false});
    BOOST_REQUIRE(disabled); BOOST_CHECK(!disabled.value().user.enabled);
    auto same = f.accounts->execute(ac, SetUserEnabledCommand{member.value().id, condition(disabled.value().user), false});
    BOOST_REQUIRE(same); BOOST_CHECK(same.value().user.revision == disabled.value().user.revision);
    auto enabled = f.accounts->execute(ac, SetUserEnabledCommand{member.value().id, condition(disabled.value().user), true});
    BOOST_REQUIRE(enabled); BOOST_CHECK(enabled.value().privateSpaceId == disabled.value().privateSpaceId);
    BOOST_CHECK(enabled.value().user.authEpoch == disabled.value().user.authEpoch);
    errorIs(f.service->execute(f.context(first), GetMeCommand{}), ErrorCode::Unauthenticated);
    errorIs(f.service->execute(f.context(second), GetMeCommand{}), ErrorCode::Unauthenticated);
    BOOST_REQUIRE(f.service->execute(ac, GetMeCommand{}));
    BOOST_REQUIRE(f.service->execute(f.context(f.login("member_one")), GetMeCommand{}));
}
BOOST_AUTO_TEST_CASE(quota_scopes_conditions_and_mkdir_ordering) {
    Fixture f; auto admin = f.login(); auto ac = f.context(admin); auto invite = f.invite(ac);
    auto member = f.registerUser("member_one", invite); BOOST_REQUIRE(member);
    auto login = f.login("member_one"); auto mc = f.context(login);
    auto views = f.service->execute(mc, ListSpacesCommand{}); BOOST_REQUIRE(views);
    CreateDirectory mkdir(*f.resources->metadata, *f.resources->clock, *f.resources->nodeIds);
    for (const auto& view : views.value()) {
        std::vector<QuotaScope> scopes{SpaceQuota{view.space.id}};
        if (view.space.kind == SpaceKind::Public) {
            scopes.push_back(PublicContributionQuota{view.space.id, member.value().id});
            scopes.push_back(PublicContributionQuota{view.space.id, admin.user.id});
        }
        for (const auto& scope : scopes) {
            errorIs(f.accounts->execute(mc, GetQuotaCommand{scope}), ErrorCode::Forbidden);
            auto before = f.accounts->execute(ac, GetQuotaCommand{scope}); BOOST_REQUIRE(before);
            errorIs(f.accounts->execute(ac, SetQuotaLimitCommand{scope, condition(admin.user), before.value().quota.state.limit}), ErrorCode::PreconditionFailed);
            auto set = f.accounts->execute(ac, SetQuotaLimitCommand{scope, condition(before.value()), before.value().quota.state.used});
            BOOST_REQUIRE(set); BOOST_CHECK(set.value().quota.state.limit.nodes == before.value().quota.state.used.nodes);
            auto same = f.accounts->execute(ac, SetQuotaLimitCommand{scope, condition(set.value()), set.value().quota.state.limit});
            BOOST_REQUIRE(same); BOOST_CHECK(same.value().quota.revision == set.value().quota.revision);
            if (set.value().quota.state.used.nodes.value())
                errorIs(f.accounts->execute(ac, SetQuotaLimitCommand{scope, condition(set.value()), {}}), ErrorCode::QuotaExceeded);
            // 恢复额度，后续目录写使用真实账户计费。
            BOOST_REQUIRE(f.accounts->execute(ac, SetQuotaLimitCommand{scope, condition(set.value()), before.value().quota.state.limit}));
        }
        auto before = f.accounts->execute(ac, GetQuotaCommand{SpaceQuota{view.space.id}}); BOOST_REQUIRE(before);
        BOOST_REQUIRE(mkdir.execute(mc, {view.space.id, view.space.rootNodeId, "child"}));
        errorIs(f.accounts->execute(ac, SetQuotaLimitCommand{before.value().scope, condition(before.value()), before.value().quota.state.limit}), ErrorCode::PreconditionFailed);
        auto current = f.accounts->execute(ac, GetQuotaCommand{before.value().scope}); BOOST_REQUIRE(current);
        BOOST_REQUIRE(f.accounts->execute(ac, SetQuotaLimitCommand{current.value().scope, condition(current.value()), current.value().quota.state.used}));
        errorIs(mkdir.execute(mc, {view.space.id, view.space.rootNodeId, "over-limit"}), ErrorCode::QuotaExceeded);
    }
}
BOOST_AUTO_TEST_CASE(password_change_commits_then_invalidates_all_sessions_and_stale_logins) {
    Fixture f; auto first = f.login(); auto second = f.login(); auto context = f.context(first);
    auto prepared = f.accounts->preparePasswordChange(context, condition(first.user)); BOOST_REQUIRE(prepared);
    auto other = f.accounts->preparePasswordChange(f.context(second), condition(second.user)); BOOST_REQUIRE(other);
    auto loginCommandValue = loginCommand();
    auto oldLogin = f.service->prepareLogin(loginCommandValue.login); BOOST_REQUIRE(oldLogin);
    errorIs(f.accounts->completePasswordChange(context, condition(first.user), prepared.value(), {}), ErrorCode::Forbidden);
    BOOST_REQUIRE(f.service->execute(context, GetMeCommand{}));
    BOOST_REQUIRE(f.accounts->completePasswordChange(context, condition(first.user), prepared.value(), newHash(f)));
    errorIs(f.accounts->completePasswordChange(f.context(second), condition(second.user), other.value(), newHash(f)), ErrorCode::Unauthenticated);
    errorIs(f.service->execute(context, GetMeCommand{}), ErrorCode::Unauthenticated);
    errorIs(f.service->execute(f.context(second), GetMeCommand{}), ErrorCode::Unauthenticated);
    errorIs(f.service->completeLogin(request(), loginCommandValue, oldLogin.value(), true), ErrorCode::Unauthenticated);
    auto now = f.service->prepareLogin(loginCommandValue.login); BOOST_REQUIRE(now && now.value());
    auto password = PasswordInput::parse("replacement password only").value();
    auto verified = f.password->hasher->verify(password, now.value()->encodedHash); BOOST_REQUIRE(verified && verified.value());
    auto fresh = f.service->completeLogin(request(), loginCommandValue, now.value(), true); BOOST_REQUIRE(fresh);
    BOOST_CHECK_EQUAL(fresh.value().user.revision.value(), 2);
    BOOST_CHECK_EQUAL(now.value()->passwordVersion.value(), 2);
}
BOOST_AUTO_TEST_CASE(password_final_phase_rechecks_rename_disable_logout_and_expiry) {
    for (unsigned mode = 0; mode != 4; ++mode) {
        Fixture f; auto admin = f.login(); auto ac = f.context(admin); auto invite = f.invite(ac);
        auto member = f.registerUser("member_one", invite); BOOST_REQUIRE(member);
        auto login = f.login("member_one"); auto mc = f.context(login);
        auto prepared = f.accounts->preparePasswordChange(mc, condition(member.value())); BOOST_REQUIRE(prepared);
        auto hash = newHash(f); // 在后续事务之外，不保留读快照。
        if (mode == 0) BOOST_REQUIRE(f.accounts->execute(mc, UpdateDisplayNameCommand{condition(member.value()), DisplayName::parse("Changed").value()}));
        if (mode == 1) BOOST_REQUIRE(f.accounts->execute(ac, SetUserEnabledCommand{member.value().id, condition(member.value()), false}));
        if (mode == 2) BOOST_REQUIRE(f.service->execute(mc, LogoutCommand{}));
        if (mode == 3) f.clock->time += std::chrono::hours(24 * 8);
        errorIs(f.accounts->completePasswordChange(mc, condition(member.value()), prepared.value(), std::move(hash)),
                mode == 0 ? ErrorCode::PreconditionFailed : ErrorCode::Unauthenticated);
        auto stored = f.service->prepareLogin(LoginName::parse("member_one").value()); BOOST_REQUIRE(stored && stored.value());
        BOOST_CHECK_EQUAL(stored.value()->passwordVersion.value(), 1);
    }
}
BOOST_AUTO_TEST_CASE(account_commit_unknown_does_not_report_success_or_retry) {
    Fixture f; auto login = f.login(); auto context = f.context(login);
    UnknownStore store(*f.resources->metadata); AccountService service(store, *f.resources->clock);
    auto result = service.execute(context, UpdateDisplayNameCommand{condition(login.user), DisplayName::parse("Committed").value()});
    errorIs(result, ErrorCode::CommitOutcomeUnknown);
    auto current = f.service->execute(context, GetMeCommand{}); BOOST_REQUIRE(current);
    BOOST_CHECK_EQUAL(current.value().revision.value(), 2);
    auto prepared = service.preparePasswordChange(context, condition(current.value())); BOOST_REQUIRE(prepared);
    errorIs(service.completePasswordChange(context, condition(current.value()), prepared.value(), newHash(f)), ErrorCode::CommitOutcomeUnknown);
    errorIs(f.service->execute(context, GetMeCommand{}), ErrorCode::Unauthenticated);
}
BOOST_AUTO_TEST_CASE(application_noops_skip_audits_and_audit_failures_roll_back_all_account_writes) {
    Fixture f; auto admin = f.login(); auto ac = f.context(admin); auto invite = f.invite(ac);
    auto member = f.registerUser("member_one", invite); BOOST_REQUIRE(member);
    auto login = f.login("member_one"); auto mc = f.context(login);
    AuditControl control; AuditedStore store(*f.resources->metadata, control); AccountService accounts(store, *f.resources->clock);
    auto view = accounts.execute(ac, GetUserCommand{member.value().id}); BOOST_REQUIRE(view);
    auto q = accounts.execute(ac, GetQuotaCommand{SpaceQuota{view.value().privateSpaceId}}); BOOST_REQUIRE(q);
    BOOST_REQUIRE(accounts.execute(mc, UpdateDisplayNameCommand{condition(member.value()), member.value().displayName}));
    BOOST_REQUIRE(accounts.execute(ac, SetUserEnabledCommand{member.value().id, condition(member.value()), true}));
    BOOST_REQUIRE(accounts.execute(ac, SetQuotaLimitCommand{q.value().scope, condition(q.value()), q.value().quota.state.limit}));
    BOOST_CHECK_EQUAL(control.calls, 0);
    control.fail = true;
    errorIs(accounts.execute(mc, UpdateDisplayNameCommand{condition(member.value()), DisplayName::parse("Rolled back").value()}), ErrorCode::InternalError);
    errorIs(accounts.execute(ac, SetUserEnabledCommand{member.value().id, condition(member.value()), false}), ErrorCode::InternalError);
    errorIs(accounts.execute(ac, SetQuotaLimitCommand{q.value().scope, condition(q.value()), q.value().quota.state.used}), ErrorCode::InternalError);
    auto prepared = accounts.preparePasswordChange(mc, condition(member.value())); BOOST_REQUIRE(prepared);
    errorIs(accounts.completePasswordChange(mc, condition(member.value()), prepared.value(), newHash(f)), ErrorCode::InternalError);
    BOOST_CHECK_EQUAL(control.calls, 4);
    auto current = f.service->execute(mc, GetMeCommand{}); BOOST_REQUIRE(current);
    BOOST_CHECK(current.value().revision == member.value().revision); BOOST_CHECK(current.value().enabled);
    auto hash = f.service->prepareLogin(LoginName::parse("member_one").value()); BOOST_REQUIRE(hash && hash.value());
    BOOST_CHECK_EQUAL(hash.value()->passwordVersion.value(), 1);
    auto quota = accounts.execute(ac, GetQuotaCommand{q.value().scope}); BOOST_REQUIRE(quota);
    BOOST_CHECK(quota.value().quota.revision == q.value().quota.revision);
    // 已写审计之后 Session 到期也必须回滚资料更新。
    control.fail = false; control.onAppend = [&] { f.clock->time += std::chrono::hours(24 * 8); };
    errorIs(accounts.execute(mc, UpdateDisplayNameCommand{condition(member.value()), DisplayName::parse("Expired").value()}), ErrorCode::Unauthenticated);
    auto read = f.resources->metadata->beginRead(); BOOST_REQUIRE(read);
    auto stored = read.value()->users().find(member.value().id); BOOST_REQUIRE(stored && stored.value());
    BOOST_CHECK(stored.value()->revision == member.value().revision);
}
