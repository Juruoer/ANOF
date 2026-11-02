#include <tests/support/EventLoopWait.h>
#define BOOST_TEST_MODULE IdentityExecutorIntegration
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/runtime/InstanceIdentity.h>
#include <muduox/net/EventLoop.h>
#include <tests/integration/InitializedInstance.h>
#include <unistd.h>

#include <atomic>
#include <boost/test/unit_test.hpp>
#include <condition_variable>
#include <mutex>
#include <typeinfo>
using namespace anof;
using namespace anof::runtime;
namespace {
bool drive(muduox::net::EventLoop& loop, const std::function<bool()>& done,
           const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
    return anof::test::waitFor(loop, done, 4, file, line);
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<unsigned> calls{ 0 }, hashes{ 0 }, verified{ 0 };
    bool block = false, released = false;
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        cv.notify_all();
    }
};
struct GatedHasher : PasswordHasher {
    std::unique_ptr<PasswordHasher> inner;
    Gate& gate;
    GatedHasher(std::unique_ptr<PasswordHasher> value, Gate& g) : inner(std::move(value)), gate(g) {}
    Result<EncodedPasswordHash> hash(const PasswordInput& input) override {
        ++gate.hashes;
        return inner->hash(input);
    }
    Result<bool> verify(const PasswordInput& input, const EncodedPasswordHash& hash) override {
        ++gate.calls;
        std::unique_lock<std::mutex> lock(gate.mutex);
        if (gate.block && !gate.cv.wait_for(lock, std::chrono::seconds(3), [&] { return gate.released; })) return ErrorCode::InternalError;
        lock.unlock();
        auto result = inner->verify(input, hash);
        ++gate.verified;
        return result;
    }
};
RequestId request() { return RequestId::parse("12345678-1234-4234-8234-123456789abc").value(); }
LoginCommand command(std::string name = "administrator") {
    return { LoginName::parse(name).value(), PasswordInput::parse("fixture password only").value(),
             CredentialMode::Bearer, AuthenticationSource::ipv4({ 127, 0, 0, 1 }) };
}
struct Fixture {
    std::filesystem::path root;
    muduox::net::EventLoop loop;
    MetadataExecutor metadata{ loop, { 8, 32 } };
    IdentityExecutor identity{ loop, metadata, { 4, 1 } };
    Gate gate;
    Fixture() {
        char pattern[] = "/tmp/anof-identity-executor-XXXXXX";
        auto* path = ::mkdtemp(pattern);
        BOOST_REQUIRE(path);
        root = path;
        BOOST_REQUIRE(anof::testing::initializeTestInstance(root));
        bool ready = false;
        BOOST_REQUIRE(metadata.start([this] { return makeInstanceMetadataResources(root / ".anof/metadata.sqlite3"); },
                                     [&](Result<void> result) { BOOST_REQUIRE(result); ready = true; }));
        BOOST_REQUIRE(drive(loop, [&] { return ready; }));
    }
    void start() {
        bool ready = false;
        BOOST_REQUIRE(identity.start([this]() -> Result<PasswordWorkerResources> {
            auto result = makeInstancePasswordResources();
            if (!result) return result.error();
            result.value().hasher = std::make_unique<GatedHasher>(std::move(result.value().hasher), gate);
            return std::move(result).value(); }, [&](Result<void> result) { BOOST_REQUIRE(result); ready = true; }));
        BOOST_REQUIRE(drive(loop, [&] { return ready; }));
    }
    ~Fixture() {
        gate.release();
        identity.requestStop();
        if (!drive(loop, [&] { return identity.stopped(); })) std::terminate();
        metadata.requestStop();
        if (!drive(loop, [&] { return metadata.snapshot().state == MetadataExecutorState::Stopped; })) std::terminate();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    template <class T, class Context, class Command>
    Result<T> execute(Context context, Command value) {
        auto result = std::make_shared<std::optional<Result<T>>>();
        Completion<T> callback = [this, result](TaskCompletion<T> completed) {
            loop.assertInLoopThread();
            if (auto outcome = std::get_if<Result<T>>(&completed.outcome))
                result->emplace(std::move(*outcome));
            else
                result->emplace(ErrorCode::Retryable);
        };
        auto submitted = identity.submit(context, std::move(value), std::move(callback));
        BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submitted));
        BOOST_CHECK(!*result);
        const bool done = drive(loop, [&] { return result->has_value(); });
        const auto state = metadata.snapshot();
        BOOST_REQUIRE_MESSAGE(done, "Missing completion for " << typeid(Command).name()
                                                              << ", verifies=" << gate.calls.load() << ", hashes=" << gate.hashes.load() << ", verified=" << gate.verified.load()
                                                              << ", metadata pending=" << state.pending << ", running=" << state.running
                                                              << ", completed=" << state.completed << ", outstanding=" << state.outstanding);
        return std::move(**result);
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(password_work_does_not_hold_metadata_and_cancel_prevents_session) {
    Fixture f;
    f.gate.block = true;
    f.start();
    bool completed = false;
    auto submitted = f.identity.submit(request(), command(), [&](TaskCompletion<LoginResult> result) {
        f.loop.assertInLoopThread();
        BOOST_CHECK(std::holds_alternative<CancelledBeforeStart>(result.outcome));
        auto reentrant = f.identity.submit(request(), command(), [](auto) {});
        BOOST_CHECK(std::get<SubmitError>(reentrant) == SubmitError::Stopping);
        completed = true;
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submitted));
    BOOST_REQUIRE(drive(f.loop, [&] { return f.gate.calls.load() == 1; }));
    auto full = f.identity.submit(request(), command(), [](auto) {});
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(full));
    BOOST_CHECK(std::get<SubmitError>(full) == SubmitError::QueueFull);
    bool metadataRan = false;
    auto other = f.metadata.submitOperation([](MetadataWorkerResources& resources) -> Result<void> {
        auto tx = resources.metadata->beginWrite(); if (!tx) return tx.error(); return tx.value()->commit(); }, [&](TaskCompletion<void> result) { BOOST_REQUIRE(std::get<Result<void>>(result.outcome)); metadataRan = true; });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(other));
    BOOST_REQUIRE(drive(f.loop, [&] { return metadataRan; }));
    BOOST_CHECK(std::get<AcceptedTask>(submitted).handle->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    f.identity.requestStop();
    BOOST_CHECK(!f.identity.stopped());
    BOOST_CHECK(!completed);
    f.gate.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.identity.stopped(); }));
    BOOST_CHECK(completed);
    BOOST_CHECK(std::get<AcceptedTask>(submitted).handle->cancelBeforeStart() == CancelBeforeStartResult::AlreadyTerminal);
    bool checked = false;
    f.metadata.submitOperation([](MetadataWorkerResources& resources) -> Result<void> {
        auto tx = resources.metadata->beginRead(); if (!tx) return tx.error();
        auto user = tx.value()->users().findPassword(LoginName::parse("administrator").value());
        if (!user || !user.value()) return ErrorCode::InternalError;
        auto count = tx.value()->sessions().countActive(user.value()->userId, resources.clock->now());
        BOOST_REQUIRE(count); BOOST_CHECK_EQUAL(count.value().value(), 0); return {}; }, [&](auto) { checked = true; });
    BOOST_REQUIRE(drive(f.loop, [&] { return checked; }));
}
BOOST_AUTO_TEST_CASE(real_login_invite_register_read_and_dummy_verify) {
    Fixture f;
    f.start();
    auto login = f.execute<LoginResult>(request(), command());
    BOOST_REQUIRE(login);
    auto codec = infrastructure::crypto::makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    auto digest = codec.value()->digestBearer(login.value().accessToken.view());
    BOOST_REQUIRE(digest);
    RequestContext context{ { digest.value(), CredentialMode::Bearer }, request() };
    auto me = f.execute<CurrentUserView>(context, GetMeCommand{});
    BOOST_REQUIRE(me);
    BOOST_CHECK(me.value().user.id == login.value().user.id);
    auto invite = f.execute<CreatedInvite>(context, CreateInviteCommand{ InviteUseLimit::from(1).value(), InviteLifetimeSeconds::from(3600).value() });
    BOOST_REQUIRE(invite);
    auto member = f.execute<UserProfile>(request(), RegisterCommand{ LoginName::parse("new_member").value(), DisplayName::parse("Member").value(),
                                                                     PasswordInput::parse("fixture password only").value(), InviteTokenText::parse(invite.value().token.view()).value(),
                                                                     AuthenticationSource::ipv4({ 127, 0, 0, 2 }) });
    BOOST_REQUIRE(member);
    BOOST_CHECK(member.value().role == UserRole::Member);
    for (int i = 0; i < 3; ++i) {
        auto unknown = f.execute<LoginResult>(request(), command("missing_user"));
        BOOST_REQUIRE(!unknown);
        BOOST_CHECK(unknown.error().code == ErrorCode::Unauthenticated);
    }
    BOOST_CHECK_EQUAL(f.gate.calls.load(), 4);
    auto limited = f.identity.submit(request(), command("missing_user"), [](auto) { BOOST_FAIL("Rejected work completed"); });
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(limited));
    BOOST_CHECK(std::get<SubmitError>(limited) == SubmitError::RateLimited);
    BOOST_REQUIRE(f.execute<void>(context, LogoutCommand{}));
}
BOOST_AUTO_TEST_CASE(password_factory_failure_is_reported_and_stops_without_stopping_metadata) {
    Fixture f;
    bool reported = false, stopped = false;
    BOOST_REQUIRE(f.identity.start([]() -> Result<PasswordWorkerResources> { return ErrorCode::InternalError; },
                                   [&](Result<void> result) { BOOST_CHECK(!result); reported = true; }, [&] { stopped = true; }));
    BOOST_REQUIRE(drive(f.loop, [&] { return stopped; }));
    BOOST_CHECK(reported);
    BOOST_CHECK(f.metadata.snapshot().state == MetadataExecutorState::Ready);
}
BOOST_AUTO_TEST_CASE(completion_retains_password_slot_and_callback_exception_is_isolated) {
    Fixture f;
    f.start();
    bool completed = false;
    auto submission = f.identity.submit(request(), command(), [&](TaskCompletion<LoginResult> result) {
        BOOST_REQUIRE(std::get<Result<LoginResult>>(result.outcome));
        auto nested = f.identity.submit(request(), command(), [](auto) {});
        BOOST_REQUIRE(std::holds_alternative<SubmitError>(nested));
        BOOST_CHECK(std::get<SubmitError>(nested) == SubmitError::QueueFull);
        completed = true;
        throw std::runtime_error("test completion failure");
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submission));
    BOOST_REQUIRE(drive(f.loop, [&] { return completed; }));
    BOOST_REQUIRE(f.execute<LoginResult>(request(), command()));
}
namespace {
RequestContext accountContext(const LoginResult& login) {
    auto codec = infrastructure::crypto::makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    auto digest = codec.value()->digestBearer(login.accessToken.view());
    BOOST_REQUIRE(digest);
    return { { digest.value(), CredentialMode::Bearer }, request() };
}
ChangePasswordCommand changeCommand(const UserProfile& user, bool correct = true, unsigned char address = 1) {
    return { UserCondition{ user.id, user.revision }, PasswordInput::parse(correct ? "fixture password only" : "incorrect password only").value(),
             PasswordInput::parse("replacement password only").value(), AuthenticationSource::ipv4({ 127, 0, 0, address }) };
}
}  // namespace
BOOST_AUTO_TEST_CASE(account_dispatch_and_password_change_use_existing_workers) {
    Fixture f;
    f.start();
    auto login = f.execute<LoginResult>(request(), command());
    BOOST_REQUIRE(login);
    auto context = accountContext(login.value());
    auto page = f.execute<Page<UserProfile, UserCursor>>(context, ListUsersCommand{ PageSize::from(1).value(), {} });
    BOOST_REQUIRE(page);
    BOOST_REQUIRE_EQUAL(page.value().items.size(), 1);
    auto user = f.execute<AdminUserView>(context, GetUserCommand{ login.value().user.id });
    BOOST_REQUIRE(user);
    auto enabled = f.execute<AdminUserView>(context, SetUserEnabledCommand{ user.value().user.id, UserCondition{ user.value().user.id, user.value().user.revision }, true });
    BOOST_REQUIRE(enabled);
    auto quota = f.execute<ManagedQuota>(context, GetQuotaCommand{ SpaceQuota{ user.value().privateSpaceId } });
    BOOST_REQUIRE(quota);
    BOOST_REQUIRE(f.execute<ManagedQuota>(context, SetQuotaLimitCommand{ quota.value().scope,
                                                                         QuotaCondition{ quota.value().scope, quota.value().quota.revision }, quota.value().quota.state.limit }));
    auto renamed = f.execute<UserProfile>(context, UpdateDisplayNameCommand{ UserCondition{ user.value().user.id, user.value().user.revision }, DisplayName::parse("Renamed").value() });
    BOOST_REQUIRE(renamed);
    BOOST_REQUIRE(f.execute<void>(context, changeCommand(renamed.value())));
    BOOST_CHECK_EQUAL(f.gate.hashes.load(), 1);
    auto stale = f.execute<CurrentUserView>(context, GetMeCommand{});
    BOOST_REQUIRE(!stale);
    BOOST_CHECK(stale.error().code == ErrorCode::Unauthenticated);
    auto next = command();
    next.password = PasswordInput::parse("replacement password only").value();
    auto loggedIn = f.execute<LoginResult>(request(), std::move(next));
    BOOST_REQUIRE(loggedIn);
    BOOST_CHECK_EQUAL(loggedIn.value().user.revision.value(), 3);
}
BOOST_AUTO_TEST_CASE(password_change_wrong_password_is_bounded_and_rate_limited_after_authentication) {
    Fixture f;
    f.start();
    auto login = f.execute<LoginResult>(request(), command());
    BOOST_REQUIRE(login);
    auto context = accountContext(login.value());
    for (unsigned n = 0; n != 2; ++n) {
        auto result = f.execute<void>(context, changeCommand(login.value().user, false));
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::Forbidden);
    }
    // 与同来源/登录名的登录共享三个突发令牌；此时任务已接纳，限流作为延后 Result 返回。
    auto limited = f.execute<void>(context, changeCommand(login.value().user, false));
    BOOST_REQUIRE(!limited);
    BOOST_CHECK(limited.error().code == ErrorCode::RateLimited);
    BOOST_CHECK_EQUAL(f.gate.calls.load(), 3);
    BOOST_CHECK_EQUAL(f.gate.hashes.load(), 0);
    BOOST_REQUIRE(f.execute<CurrentUserView>(context, GetMeCommand{}));
    auto invalid = context;
    invalid.credential.digest = SessionDigest{ SessionDigest::Bytes{ 7 } };
    auto rejected = f.execute<void>(invalid, changeCommand(login.value().user, false));
    BOOST_REQUIRE(!rejected);
    BOOST_CHECK(rejected.error().code == ErrorCode::Unauthenticated);
    BOOST_CHECK_EQUAL(f.gate.calls.load(), 3);
}
BOOST_AUTO_TEST_CASE(password_change_gate_allows_metadata_and_rejects_stale_revision) {
    Fixture f;
    f.start();
    auto login = f.execute<LoginResult>(request(), command());
    BOOST_REQUIRE(login);
    auto context = accountContext(login.value());
    f.gate.block = true;
    std::optional<Result<void>> completed;
    auto task = f.identity.submit(context, changeCommand(login.value().user), [&](TaskCompletion<void> result) {
        f.loop.assertInLoopThread();
        completed.emplace(std::move(std::get<Result<void>>(result.outcome)));
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(task));
    BOOST_REQUIRE(drive(f.loop, [&] { return f.gate.calls.load() == 2; }));
    auto full = f.identity.submit(context, changeCommand(login.value().user), [](auto) { BOOST_FAIL("Rejected task completed"); });
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(full));
    BOOST_CHECK(std::get<SubmitError>(full) == SubmitError::QueueFull);
    auto renamed = f.execute<UserProfile>(context, UpdateDisplayNameCommand{ UserCondition{ login.value().user.id, login.value().user.revision }, DisplayName::parse("Concurrent rename").value() });
    BOOST_REQUIRE(renamed);
    BOOST_CHECK(!completed);
    f.gate.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return completed.has_value(); }));
    BOOST_REQUIRE(!*completed);
    BOOST_CHECK(completed->error().code == ErrorCode::PreconditionFailed);
    BOOST_REQUIRE(f.execute<CurrentUserView>(context, GetMeCommand{}));
}
BOOST_AUTO_TEST_CASE(password_change_cancel_and_shutdown_wait_for_password_worker) {
    Fixture f;
    f.start();
    auto login = f.execute<LoginResult>(request(), command());
    BOOST_REQUIRE(login);
    auto context = accountContext(login.value());
    f.gate.block = true;
    unsigned completed = 0;
    auto task = f.identity.submit(context, changeCommand(login.value().user), [&](TaskCompletion<void> result) {
        f.loop.assertInLoopThread();
        BOOST_CHECK(std::holds_alternative<CancelledBeforeStart>(result.outcome));
        ++completed;
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(task));
    BOOST_REQUIRE(drive(f.loop, [&] { return f.gate.calls.load() == 2; }));
    BOOST_CHECK(std::get<AcceptedTask>(task).handle->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    f.identity.requestStop();
    BOOST_CHECK(!f.identity.stopped());
    BOOST_CHECK_EQUAL(completed, 0);
    f.gate.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.identity.stopped(); }));
    BOOST_CHECK_EQUAL(completed, 1);
    BOOST_CHECK_EQUAL(f.gate.hashes.load(), 0);
    bool checked = false;
    auto verification = f.metadata.submitOperation([&](MetadataWorkerResources& resources) -> Result<void> {
        auto read = resources.metadata->beginRead(); if (!read) return read.error();
        auto active = read.value()->authorization().requireActiveSession(context.credential, resources.clock->now());
        if (!active) return active.error();
        auto user = read.value()->users().find(login.value().user.id); if (!user) return user.error();
        BOOST_REQUIRE(user.value()); BOOST_CHECK_EQUAL(user.value()->revision.value(), 1); return {}; }, [&](TaskCompletion<void> result) { BOOST_REQUIRE(std::get<Result<void>>(result.outcome)); checked = true; });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(verification));
    BOOST_REQUIRE(drive(f.loop, [&] { return checked; }));
}

BOOST_AUTO_TEST_CASE(cookie_login_and_current_user_use_the_existing_bounded_workers) {
    Fixture f;
    f.start();
    auto cmd = command();
    cmd.mode = CredentialMode::Cookie;
    auto login = f.execute<LoginResult>(request(), std::move(cmd));
    BOOST_REQUIRE(login);
    auto codec = infrastructure::crypto::makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    auto digest = codec.value()->digestBearer(login.value().accessToken.view());
    BOOST_REQUIRE(digest);
    RequestContext context{ { digest.value(), CredentialMode::Cookie }, request() };
    auto me = f.execute<CurrentUserView>(context, GetMeCommand{});
    BOOST_REQUIRE(me);
    BOOST_CHECK(me.value().user.id == login.value().user.id);
    context.credential.mode = CredentialMode::Bearer;
    auto mismatch = f.execute<CurrentUserView>(context, GetMeCommand{});
    BOOST_REQUIRE(!mismatch);
    BOOST_CHECK(mismatch.error().code == ErrorCode::Unauthenticated);
    context.credential.mode = CredentialMode::Cookie;
    BOOST_REQUIRE(f.execute<void>(context, LogoutCommand{}));
    BOOST_CHECK(!f.execute<CurrentUserView>(context, GetMeCommand{}));
}
