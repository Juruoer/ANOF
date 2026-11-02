#include <anof/runtime/internal/ShutdownTrace.h>
#include <anof/runtime/IdentityExecutor.h>
#include <anof/runtime/AuthenticationLimiter.h>
#include <anof/runtime/CompletionPump.h>
#include <anof/application/identity/IdentityService.h>
#include <anof/application/identity/AccountService.h>
#include <muduox/net/EventLoop.h>
#include <atomic>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace anof::runtime {
namespace {
enum class State { Active, Cancelled, Running, Terminal };
enum class Phase { Read, Password, Final };
struct Control {
    std::atomic<State> state{State::Active};
    std::uint64_t id;
    explicit Control(std::uint64_t value) : id(value) {}
};
class Handle final : public TaskHandle {
public:
    explicit Handle(std::shared_ptr<Control> value) : control_(std::move(value)) {}
    CancelBeforeStartResult cancelBeforeStart() noexcept override {
        auto expected = State::Active;
        if (control_->state.compare_exchange_strong(expected, State::Cancelled)) return CancelBeforeStartResult::Cancelled;
        return expected == State::Running ? CancelBeforeStartResult::AlreadyRunning : CancelBeforeStartResult::AlreadyTerminal;
    }
    std::uint64_t id() const noexcept override { return control_->id; }
private:
    std::shared_ptr<Control> control_;
};
struct Job {
    std::shared_ptr<Control> control;
    Phase phase;
    bool passwordRequest;
    bool ready = false; // mutex 保护；也是 worker 数据向 loop 的发布屏障。
    bool failed = false;
    Job(std::shared_ptr<Control> c, Phase p, bool password) : control(std::move(c)), phase(p), passwordRequest(password) {}
    virtual ~Job() = default;
    virtual ErrorCode exceptionCode() const noexcept {
        return phase == Phase::Read ? ErrorCode::InternalError : ErrorCode::CommitOutcomeUnknown;
    }
    virtual void metadata(IdentityService&, AccountService&) = 0;
    virtual bool admitPassword(AuthenticationLimiter&) { return true; }
    virtual bool cleanupBeforeFinal() const noexcept { return passwordRequest; }
    virtual void password(PasswordWorkerResources&) {}
    virtual void fail(ErrorCode) noexcept = 0;
    virtual void deliver() = 0;
};
template<class T> struct TypedJob : Job {
    Completion<T> callback;
    Result<T> result{ErrorCode::InternalError};
    TypedJob(std::shared_ptr<Control> c, Phase p, bool password, Completion<T> cb)
        : Job(std::move(c), p, password), callback(std::move(cb)) {}
    void fail(ErrorCode error) noexcept override { result = error; this->failed = true; }
    void deliver() override {
        const auto previous = this->control->state.exchange(State::Terminal);
        if (previous == State::Cancelled) callback({this->control->id, CancelledBeforeStart{}});
        else callback({this->control->id, std::move(result)});
    }
};
template<class C, class T, bool Account = false> struct SimpleJob final : TypedJob<T> {
    RequestContext context;
    C command;
    SimpleJob(std::shared_ptr<Control> c, RequestContext ctx, C cmd, Completion<T> cb)
        : TypedJob<T>(std::move(c), Phase::Final, false, std::move(cb)), context(std::move(ctx)), command(std::move(cmd)) {}
    ErrorCode exceptionCode() const noexcept override {
        constexpr bool writes = std::is_same_v<C, LogoutCommand> || std::is_same_v<C, CreateInviteCommand> || std::is_same_v<C, RevokeInviteCommand> ||
            std::is_same_v<C, UpdateDisplayNameCommand> || std::is_same_v<C, SetUserEnabledCommand> || std::is_same_v<C, SetQuotaLimitCommand>;
        return writes ? ErrorCode::CommitOutcomeUnknown : ErrorCode::InternalError;
    }
    void metadata(IdentityService& service, AccountService& accounts) override {
        if constexpr (Account) this->result = accounts.execute(context, std::move(command));
        else this->result = service.execute(context, std::move(command));
    }
};
struct LoginJob final : TypedJob<LoginResult> {
    RequestId request;
    LoginCommand command;
    std::optional<PasswordSnapshot> snapshot;
    bool verified = false;
    LoginJob(std::shared_ptr<Control> c, RequestId id, LoginCommand cmd, Completion<LoginResult> cb)
        : TypedJob(std::move(c), Phase::Read, true, std::move(cb)), request(id), command(std::move(cmd)) {}
    void metadata(IdentityService& service, AccountService&) override {
        if (phase == Phase::Read) {
            auto read = service.prepareLogin(command.login);
            if (!read) fail(read.error().code);
            else snapshot = std::move(read).value();
        } else result = service.completeLogin(request, command, snapshot, verified);
    }
    void password(PasswordWorkerResources& resources) override {
        auto checked = resources.hasher->verify(command.password, snapshot ? snapshot->encodedHash : resources.dummy);
        if (!checked) fail(checked.error().code);
        else verified = checked.value();
    }
};
struct RegisterJob final : TypedJob<UserProfile> {
    RequestId request;
    RegisterCommand command;
    std::optional<EncodedPasswordHash> hash;
    RegisterJob(std::shared_ptr<Control> c, RequestId id, RegisterCommand cmd, Completion<UserProfile> cb)
        : TypedJob(std::move(c), Phase::Password, true, std::move(cb)), request(id), command(std::move(cmd)) {}
    void metadata(IdentityService& service, AccountService&) override { result = service.registerUser(request, command, std::move(*hash)); }
    void password(PasswordWorkerResources& resources) override {
        auto generated = resources.hasher->hash(command.password);
        if (!generated) fail(generated.error().code);
        else hash.emplace(std::move(generated).value());
    }
};
struct ChangePasswordJob final : TypedJob<void> {
    RequestContext context;
    AccountCondition expected;
    AuthenticationSource source;
    std::optional<PasswordInput> currentPassword, newPassword;
    std::optional<PasswordChangePreparation> prepared;
    std::optional<EncodedPasswordHash> hash;
    ChangePasswordJob(std::shared_ptr<Control> c, RequestContext ctx, ChangePasswordCommand cmd, Completion<void> cb)
        : TypedJob(std::move(c), Phase::Read, true, std::move(cb)), context(std::move(ctx)),
          expected(std::move(cmd.expected)), source(cmd.source), currentPassword(std::move(cmd.currentPassword)),
          newPassword(std::move(cmd.newPassword)) {}
    bool cleanupBeforeFinal() const noexcept override { return false; }
    bool admitPassword(AuthenticationLimiter& limiter) override {
        return limiter.allow(source, prepared->login, std::chrono::steady_clock::now());
    }
    void metadata(IdentityService&, AccountService& service) override {
        if (phase == Phase::Read) {
            auto value = service.preparePasswordChange(context, expected);
            if (!value) fail(value.error().code);
            else prepared.emplace(std::move(value).value());
        } else result = service.completePasswordChange(context, expected, *prepared, std::move(hash));
    }
    void password(PasswordWorkerResources& resources) override {
        auto verified = resources.hasher->verify(*currentPassword, prepared->password.encodedHash);
        currentPassword.reset();
        if (!verified) { newPassword.reset(); fail(verified.error().code); return; }
        // 错密码也回到最终认证阶段；只有验证通过且未取消才计算新哈希。
        if (verified.value() && control->state.load() != State::Cancelled) {
            auto generated = resources.hasher->hash(*newPassword);
            if (!generated) fail(generated.error().code);
            else hash.emplace(std::move(generated).value());
        }
        newPassword.reset();
    }
};
IdentityExecutorOptions checked(IdentityExecutorOptions value) {
    if (!value.maxOutstanding || value.maxOutstanding > 128 || !value.maxPasswordRequests ||
        value.maxPasswordRequests > 8 || value.maxPasswordRequests > value.maxOutstanding)
        throw std::invalid_argument("Invalid identity executor capacity");
    return value;
}
} // namespace

struct IdentityExecutor::Impl {
    muduox::net::EventLoop& loop;
    MetadataExecutor& metadata;
    IdentityExecutorOptions options;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<Job>> jobs; // loop 独占，固定槽；工作线程仅持有自己的 shared_ptr。
    std::vector<std::shared_ptr<Job>> passwords;
    std::size_t head = 0, count = 0, outstanding = 0, passwordCount = 0;
    std::uint64_t nextId = 1;
    bool stopping = false, startupAvailable = false, startupDelivered = false, workerFinished = false;
    MetadataExecutorState state = MetadataExecutorState::Created;
    Result<void> startupResult;
    MetadataExecutor::Started onStarted;
    MetadataExecutor::Stopped onStopped;
    std::function<void(UserId, bool)> userDisabled;
    std::thread worker;
    std::promise<void> exitPromise;
    std::future<void> exited = exitPromise.get_future();
    AuthenticationLimiter limiter;
    std::chrono::steady_clock::time_point nextCleanup{}; // 仅 metadata worker 使用。
    CompletionPump pump;
    Impl(muduox::net::EventLoop& owner, MetadataExecutor& executor, IdentityExecutorOptions limits)
        : loop(owner), metadata(executor), options(checked(limits)), jobs(options.maxOutstanding),
          passwords(options.maxPasswordRequests), pump(owner, [this] { return drain(); }, [this] { finish(); }) {}
    void publish(std::shared_ptr<Job> job) noexcept {
        { std::lock_guard<std::mutex> lock(mutex); job->ready = true; }
        job.reset(); // worker 不在发布后继续持有任务，闭包与容量由 loop 统一归还。
        pump.notify();
    }
    void startup(Result<void> result) noexcept {
        { std::lock_guard<std::mutex> lock(mutex); startupResult = result; startupAvailable = true; if (!result) stopping = true; }
        pump.notify();
    }
    void work(Factory factory) noexcept {
        bool registered = false;
        try { exitPromise.set_value_at_thread_exit(); registered = true; }
        catch (...) {}
        if (!registered) { factory = {}; startup(ErrorCode::InternalError); }
        else {
            try {
                auto opened = factory(); factory = {};
                if (!opened) startup(opened.error());
                else if (!opened.value().hasher) startup(ErrorCode::InternalError);
                else {
                    auto resources = std::move(opened).value();
                    startup({});
                    for (;;) {
                        std::shared_ptr<Job> job;
                        {
                            std::unique_lock<std::mutex> lock(mutex);
                            changed.wait(lock, [this] { return stopping || count; });
                            if (!count) break;
                            job = std::move(passwords[head]); head = (head + 1) % passwords.size(); --count;
                        }
                        if (job->control->state.load() != State::Cancelled) {
                            try { job->password(resources); } catch (...) { job->fail(ErrorCode::InternalError); }
                        }
                        publish(std::move(job));
                    }
                }
            } catch (...) { factory = {}; startup(ErrorCode::InternalError); }
        }
        { std::lock_guard<std::mutex> lock(mutex); workerFinished = true; }
        internal::shutdownTrace("identity", "worker_final_notify", &pump);
        pump.notify();
        internal::shutdownTrace("identity", "worker_returning", &pump);
        if (!registered) exitPromise.set_value();
    }
    void queuePassword(const std::shared_ptr<Job>& job) noexcept {
        { std::lock_guard<std::mutex> lock(mutex);
          passwords[(head + count) % passwords.size()] = job; ++count; }
        changed.notify_one();
    }
    void queueMetadata(const std::shared_ptr<Job>& job) noexcept {
        try {
            auto submission = metadata.submitOperation([this, job](MetadataWorkerResources& resources) -> Result<void> {
                if (job->phase == Phase::Final) {
                    auto expected = State::Active;
                    if (!job->control->state.compare_exchange_strong(expected, State::Running)) return {};
                } else if (job->control->state.load() == State::Cancelled) return {};
                if (!resources.identity || !resources.identity->ids || !resources.identity->issuer || !resources.identity->invites)
                    return ErrorCode::InternalError;
                IdentityService service(*resources.metadata, *resources.clock, *resources.identity->ids,
                                        *resources.identity->issuer, *resources.identity->invites);
                // 登录触发有界清理，独立短事务；不会持有事务等待密码。
                if (job->cleanupBeforeFinal() && job->phase == Phase::Final) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= nextCleanup) {
                        nextCleanup = now + std::chrono::minutes(1);
                        auto cleaned = service.cleanupSessions();
                        if (!cleaned) return cleaned.error();
                    }
                }
                AccountService accounts(*resources.metadata, *resources.clock);
                job->metadata(service, accounts);
                return {};
            }, [this, job](TaskCompletion<void> completion) {
                if (auto result = std::get_if<Result<void>>(&completion.outcome)) {
                    if (!*result) job->fail(result->error().code);
                } else job->fail(ErrorCode::Retryable);
                publish(job);
            }, job->exceptionCode());
            if (std::holds_alternative<SubmitError>(submission)) { job->fail(ErrorCode::Retryable); publish(job); }
        } catch (...) { job->fail(ErrorCode::InternalError); publish(job); }
    }
    bool drain() noexcept {
        loop.assertInLoopThread();
        bool report; Result<void> result;
        { std::lock_guard<std::mutex> lock(mutex); report = startupAvailable && !startupDelivered;
          if (report) { startupDelivered = true; result = startupResult; } }
        if (report) {
            if (!result) state = MetadataExecutorState::Stopping;
            else if (state == MetadataExecutorState::Starting) state = MetadataExecutorState::Ready;
            auto callback = std::move(onStarted);
            try { if (callback) callback(result); } catch (...) { stop(); }
        }
        std::size_t drained = 0;
        for (auto& slot : jobs) {
            if (drained == 32) return true;
            if (!slot) continue;
            bool ready;
            { std::lock_guard<std::mutex> lock(mutex); ready = slot->ready; slot->ready = false; }
            if (!ready) continue;
            ++drained;
            auto job = slot;
            if (!job->failed && job->control->state.load() != State::Cancelled && job->phase != Phase::Final) {
                if (job->phase == Phase::Read) {
                    try { if (!job->admitPassword(limiter)) job->fail(ErrorCode::RateLimited); }
                    catch (...) { job->fail(ErrorCode::InternalError); }
                    if (job->failed) publish(job);
                    else { job->phase = Phase::Password; queuePassword(job); }
                }
                else { job->phase = Phase::Final; queueMetadata(job); }
            } else {
                try { job->deliver(); } catch (...) {}
                // 完成及闭包销毁后才归还外层名额，防止回调重入绕开限制。
                const bool password = job->passwordRequest;
                slot.reset(); job.reset(); --outstanding; if (password) --passwordCount;
            }
        }
        return false; // notify 在 drain 期间发生时，pump 自身负责再次唤醒。
    }
    void finish() noexcept {
        if (state != MetadataExecutorState::Stopping || outstanding || !pump.idle()) return;
        { std::lock_guard<std::mutex> lock(mutex); if (!workerFinished || !startupDelivered) return; }
        if (exited.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            internal::shutdownTrace("identity", "exit_future_pending", &pump);
            return;
        }
        internal::shutdownTrace("identity", "exit_future_ready", &pump);
        if (worker.joinable()) worker.join();
        state = MetadataExecutorState::Stopped;
        internal::shutdownTrace("identity", "stopped", &pump);
        auto callback = std::move(onStopped);
        try { if (callback) callback(); } catch (...) {}
    }
    void stop() {
        loop.assertInLoopThread();
        if (state == MetadataExecutorState::Stopped || state == MetadataExecutorState::Stopping) return;
        if (state == MetadataExecutorState::Created) { state = MetadataExecutorState::Stopped; return; }
        internal::shutdownTrace("identity", "stop_requested", &pump);
        state = MetadataExecutorState::Stopping;
        for (const auto& job : jobs) if (job) { auto expected = State::Active; job->control->state.compare_exchange_strong(expected, State::Cancelled); }
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        changed.notify_one(); pump.notify();
    }
    template<class J, class Context, class Command, class T>
    TaskSubmission submit(Context context, Command command, Completion<T> callback) {
        loop.assertInLoopThread();
        if (state != MetadataExecutorState::Ready) return SubmitError::Stopping;
        if (!callback || nextId == std::numeric_limits<std::uint64_t>::max()) return SubmitError::InternalError;
        constexpr bool password = std::is_same_v<J, LoginJob> || std::is_same_v<J, RegisterJob> || std::is_same_v<J, ChangePasswordJob>;
        if (outstanding == jobs.size() || (password && passwordCount == options.maxPasswordRequests)) return SubmitError::QueueFull;
        try {
            if constexpr (std::is_same_v<J, LoginJob> || std::is_same_v<J, RegisterJob>) if (!limiter.allow(command.source, command.login, std::chrono::steady_clock::now())) return SubmitError::RateLimited;
            auto control = std::make_shared<Control>(nextId);
            auto handle = std::make_unique<Handle>(control);
            auto job = std::make_shared<J>(control, std::move(context), std::move(command), std::move(callback));
            for (auto& slot : jobs) if (!slot) { slot = job; break; }
            ++nextId; ++outstanding; if constexpr (password) ++passwordCount;
            if (job->phase == Phase::Password) queuePassword(job); else queueMetadata(job);
            return AcceptedTask{std::move(handle)};
        } catch (...) { return SubmitError::InternalError; }
    }
};
IdentityExecutor::IdentityExecutor(muduox::net::EventLoop& loop, MetadataExecutor& metadata, IdentityExecutorOptions options)
    : impl_(std::make_unique<Impl>(loop, metadata, options)) {}
IdentityExecutor::~IdentityExecutor() { impl_->loop.assertInLoopThread(); if (impl_->worker.joinable() || impl_->outstanding) std::terminate(); }
Result<void> IdentityExecutor::start(Factory factory, MetadataExecutor::Started started, MetadataExecutor::Stopped stoppedCallback) {
    auto& self = *impl_; self.loop.assertInLoopThread();
    if (!factory || self.state != MetadataExecutorState::Created || self.metadata.snapshot().state != MetadataExecutorState::Ready)
        return ErrorCode::InvalidArgument;
    self.onStarted = std::move(started); self.onStopped = std::move(stoppedCallback); self.state = MetadataExecutorState::Starting;
    try { self.worker = std::thread([&self, factory = std::move(factory)]() mutable { self.work(std::move(factory)); }); }
    catch (...) { self.state = MetadataExecutorState::Stopped; self.onStarted = {}; self.onStopped = {}; return ErrorCode::InternalError; }
    return {};
}
void IdentityExecutor::requestStop() { impl_->stop(); }
bool IdentityExecutor::stopped() const { impl_->loop.assertInLoopThread(); return impl_->state == MetadataExecutorState::Stopped; }
TaskSubmission IdentityExecutor::submit(RequestId id, RegisterCommand command, Completion<UserProfile> callback) {
    return impl_->submit<RegisterJob>(id, std::move(command), std::move(callback));
}
TaskSubmission IdentityExecutor::submit(RequestId id, LoginCommand command, Completion<LoginResult> callback) {
    return impl_->submit<LoginJob>(id, std::move(command), std::move(callback));
}
TaskSubmission IdentityExecutor::submit(RequestContext context, ChangePasswordCommand command, Completion<void> callback) {
    return impl_->submit<ChangePasswordJob>(std::move(context), std::move(command), std::move(callback));
}
#define ANOF_ACCOUNT_SUBMIT(Command, ...) \
TaskSubmission IdentityExecutor::submit(RequestContext context, Command command, Completion<__VA_ARGS__> callback) { \
    return impl_->submit<SimpleJob<Command, __VA_ARGS__, true>>(std::move(context), std::move(command), std::move(callback)); \
}
ANOF_ACCOUNT_SUBMIT(UpdateDisplayNameCommand, UserProfile)
ANOF_ACCOUNT_SUBMIT(ListUsersCommand, Page<UserProfile, UserCursor>)
ANOF_ACCOUNT_SUBMIT(GetUserCommand, AdminUserView)
void IdentityExecutor::setUserDisabledObserver(std::function<void(UserId, bool)> observer) {
    impl_->loop.assertInLoopThread();impl_->userDisabled=std::move(observer);
}
TaskSubmission IdentityExecutor::submit(RequestContext context,SetUserEnabledCommand command,Completion<AdminUserView> callback) {
    if(!callback)return SubmitError::InternalError;
    const bool disabling=!command.enabled;
    return impl_->submit<SimpleJob<SetUserEnabledCommand,AdminUserView,true>>(std::move(context),std::move(command),
        Completion<AdminUserView>{[this,disabling,user=command.id,callback=std::move(callback)](auto completion){
            // 未知提交也提示协调器读取持久标志；通知不替代事务判断。
            if(disabling && impl_->userDisabled) {
                auto* result = std::get_if<Result<AdminUserView>>(&completion.outcome);
                if(result && (*result || result->error().code == ErrorCode::CommitOutcomeUnknown))
                    impl_->userDisabled(user, bool(*result));
            }
            callback(std::move(completion));
        }});
}
ANOF_ACCOUNT_SUBMIT(GetQuotaCommand, ManagedQuota)
ANOF_ACCOUNT_SUBMIT(SetQuotaLimitCommand, ManagedQuota)
#undef ANOF_ACCOUNT_SUBMIT
#define ANOF_IDENTITY_SUBMIT(Command, ...) \
TaskSubmission IdentityExecutor::submit(RequestContext context, Command command, Completion<__VA_ARGS__> callback) { \
    return impl_->submit<SimpleJob<Command, __VA_ARGS__>>(std::move(context), std::move(command), std::move(callback)); \
}
ANOF_IDENTITY_SUBMIT(LogoutCommand, void)
ANOF_IDENTITY_SUBMIT(GetMeCommand, CurrentUserView)
ANOF_IDENTITY_SUBMIT(CreateInviteCommand, CreatedInvite)
ANOF_IDENTITY_SUBMIT(ListInvitesCommand, Page<InviteInfo, InviteCursor>)
ANOF_IDENTITY_SUBMIT(RevokeInviteCommand, void)
ANOF_IDENTITY_SUBMIT(ListSpacesCommand, std::vector<SpaceView>)
ANOF_IDENTITY_SUBMIT(GetNodeCommand, NodeDetails)
ANOF_IDENTITY_SUBMIT(ListChildrenCommand, Page<NodeDetails, ChildrenCursor>)
#undef ANOF_IDENTITY_SUBMIT
} // namespace anof::runtime
