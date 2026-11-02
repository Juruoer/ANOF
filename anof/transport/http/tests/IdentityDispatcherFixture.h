#pragma once
#include <anof/application/ports/IdentityDispatcher.h>
#include <muduox/net/EventLoop.h>

#include <boost/test/unit_test.hpp>
#include <optional>

namespace anof::http::testing {
// 所有操作在同一测试 loop；门控阶段模拟密码工作，但不启动线程或访问数据库。
class IdentityDispatcherFixture final : public IdentityDispatcher {
public:
    enum class Phase { Queued,
                       Password,
                       FinalTransaction,
                       Ready,
                       Delivered };
    struct Job {
        std::uint64_t id;
        Phase phase = Phase::Queued;
        bool cancelled = false;
        virtual ~Job() = default;
        virtual void deliver() = 0;
    };
    template <typename Command, typename Value, typename Context = RequestContext>
    struct TypedJob final : Job {
        Context context;
        std::optional<Command> command;
        Completion<Value> completion;
        std::optional<Result<Value>> result;
        TypedJob(Context ctx, Command cmd, Completion<Value> callback)
            : context(ctx), command(std::move(cmd)), completion(std::move(callback)) {}
        void deliver() override {
            command.reset();  // 模拟 worker 已结束，任务命令不遗留密码。
            auto callback = std::move(completion);
            if (this->cancelled)
                callback({ this->id, CancelledBeforeStart{} });
            else
                callback({ this->id, result ? std::move(*result) : Result<Value>(ErrorCode::InternalError) });
            result.reset();
        }
    };
    explicit IdentityDispatcherFixture(muduox::net::EventLoop& loop) : loop_(loop) {}
    std::optional<SubmitError> rejection;
    bool throwOnSubmit = false;
    std::size_t capacity = 128;
    std::size_t submissions = 0;
    std::size_t deliveries = 0;
    std::size_t cancellations = 0;
    std::vector<std::shared_ptr<Job>> jobs;
    bool startPassword(const std::shared_ptr<Job>& job) {
        loop_.assertInLoopThread();
        if (job->cancelled || job->phase != Phase::Queued) return false;
        job->phase = Phase::Password;
        return true;
    }
    bool startFinal(const std::shared_ptr<Job>& job) {
        loop_.assertInLoopThread();
        if (job->cancelled || (job->phase != Phase::Queued && job->phase != Phase::Password)) return false;
        job->phase = Phase::FinalTransaction;
        return true;
    }
    void release(const std::shared_ptr<Job>& job) {
        loop_.assertInLoopThread();
        if (job->phase == Phase::Ready || job->phase == Phase::Delivered) return;
        job->phase = Phase::Ready;
        loop_.queueInLoop([this, job] {
            job->phase = Phase::Delivered;
            job->deliver();
            ++deliveries;  // 完成回调归还后才释放名额。
        });
    }
    void releaseAll() {
        for (const auto& job : jobs) release(job);
    }
    TaskSubmission submit(RequestId context, RegisterCommand command, Completion<UserProfile> completion) override {
        return accept<RegisterCommand, UserProfile>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestId context, LoginCommand command, Completion<LoginResult> completion) override {
        return accept<LoginCommand, LoginResult>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, LogoutCommand command, Completion<void> completion) override {
        return accept<LogoutCommand, void>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, GetMeCommand command, Completion<CurrentUserView> completion) override {
        return accept<GetMeCommand, CurrentUserView>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, CreateInviteCommand command, Completion<CreatedInvite> completion) override {
        return accept<CreateInviteCommand, CreatedInvite>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, ListInvitesCommand command, Completion<Page<InviteInfo, InviteCursor>> completion) override {
        return accept<ListInvitesCommand, Page<InviteInfo, InviteCursor>>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, RevokeInviteCommand command, Completion<void> completion) override {
        return accept<RevokeInviteCommand, void>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, ListSpacesCommand command, Completion<std::vector<SpaceView>> completion) override {
        return accept<ListSpacesCommand, std::vector<SpaceView>>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, GetNodeCommand command, Completion<NodeDetails> completion) override {
        return accept<GetNodeCommand, NodeDetails>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, ListChildrenCommand command, Completion<Page<NodeDetails, ChildrenCursor>> completion) override {
        return accept<ListChildrenCommand, Page<NodeDetails, ChildrenCursor>>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, UpdateDisplayNameCommand command, Completion<UserProfile> completion) override {
        return accept<UpdateDisplayNameCommand, UserProfile>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, ChangePasswordCommand command, Completion<void> completion) override {
        return accept<ChangePasswordCommand, void>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, ListUsersCommand command, Completion<Page<UserProfile, UserCursor>> completion) override {
        return accept<ListUsersCommand, Page<UserProfile, UserCursor>>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, GetUserCommand command, Completion<AdminUserView> completion) override {
        return accept<GetUserCommand, AdminUserView>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, SetUserEnabledCommand command, Completion<AdminUserView> completion) override {
        return accept<SetUserEnabledCommand, AdminUserView>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, GetQuotaCommand command, Completion<ManagedQuota> completion) override {
        return accept<GetQuotaCommand, ManagedQuota>(context, std::move(command), std::move(completion));
    }
    TaskSubmission submit(RequestContext context, SetQuotaLimitCommand command, Completion<ManagedQuota> completion) override {
        return accept<SetQuotaLimitCommand, ManagedQuota>(context, std::move(command), std::move(completion));
    }

private:
    struct Handle final : TaskHandle {
        IdentityDispatcherFixture& owner;
        std::shared_ptr<Job> job;
        Handle(IdentityDispatcherFixture& value, std::shared_ptr<Job> task) : owner(value), job(std::move(task)) {}
        CancelBeforeStartResult cancelBeforeStart() noexcept override {
            owner.loop_.assertInLoopThread();
            ++owner.cancellations;
            if (job->phase == Phase::FinalTransaction) return CancelBeforeStartResult::AlreadyRunning;
            if (job->phase == Phase::Ready || job->phase == Phase::Delivered || job->cancelled) return CancelBeforeStartResult::AlreadyTerminal;
            job->cancelled = true;
            return CancelBeforeStartResult::Cancelled;
        }
        std::uint64_t id() const noexcept override { return job->id; }
    };
    template <typename Command, typename Value, typename Context>
    TaskSubmission accept(Context context, Command command, Completion<Value> completion) {
        loop_.assertInLoopThread();
        ++submissions;
        if (throwOnSubmit) throw std::runtime_error("Injected pre-admission failure");
        if (rejection) return *rejection;
        if (jobs.size() - deliveries >= capacity) return SubmitError::QueueFull;
        auto job = std::make_shared<TypedJob<Command, Value, Context>>(context, std::move(command), std::move(completion));
        job->id = jobs.size() + 1;
        jobs.push_back(job);
        return AcceptedTask{ std::make_unique<Handle>(*this, job) };
    }
    muduox::net::EventLoop& loop_;
};
}  // namespace anof::http::testing
