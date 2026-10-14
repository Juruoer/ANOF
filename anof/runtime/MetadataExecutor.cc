#include <anof/runtime/internal/ShutdownTrace.h>
#include <anof/runtime/MetadataExecutor.h>
#include <anof/runtime/CompletionPump.h>

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

enum class TaskState { Queued, Cancelled, Running, Terminal };

struct TaskControl {
    std::atomic<TaskState> state{ TaskState::Queued };
    std::uint64_t id;
    explicit TaskControl(std::uint64_t value) : id(value) {}
};

class Handle final : public TaskHandle {
public:
    explicit Handle(std::shared_ptr<TaskControl> control) : control_(std::move(control)) {}
    CancelBeforeStartResult cancelBeforeStart() noexcept override {
        auto expected = TaskState::Queued;
        if (control_->state.compare_exchange_strong(expected, TaskState::Cancelled)) return CancelBeforeStartResult::Cancelled;
        return expected == TaskState::Running ? CancelBeforeStartResult::AlreadyRunning : CancelBeforeStartResult::AlreadyTerminal;
    }
    std::uint64_t id() const noexcept override { return control_->id; }

private:
    std::shared_ptr<TaskControl> control_;
};

struct Job {
    std::shared_ptr<TaskControl> control;
    explicit Job(std::shared_ptr<TaskControl> value) : control(std::move(value)) {}
    virtual ~Job() = default;
    virtual void execute(MetadataWorkerResources&) = 0;
    virtual void failed() noexcept = 0;
    virtual void cancelled() noexcept = 0;
    virtual void deliver() = 0;
};
struct DirectoryJob final : Job {
    RequestContext context;
    CreateDirectoryCommand command;
    CreateDirectoryCallback callback;
    CreateDirectoryCompletion completion;
    DirectoryJob(RequestContext c, CreateDirectoryCommand cmd, CreateDirectoryCallback cb, std::shared_ptr<TaskControl> task)
        : Job(std::move(task)), context(std::move(c)), command(std::move(cmd)), callback(std::move(cb)),
          completion{control->id, Result<CreateDirectoryResult>(ErrorCode::InternalError), std::nullopt} {}
    void execute(MetadataWorkerResources& resources) override {
        CreateDirectory useCase(*resources.metadata, *resources.clock, *resources.nodeIds);
        completion.outcome = useCase.execute(context, command, &completion.attempt);
    }
    void failed() noexcept override { completion.outcome = Result<CreateDirectoryResult>(ErrorCode::CommitOutcomeUnknown); }
    void cancelled() noexcept override { completion.outcome = CancelledBeforeStart{}; }
    void deliver() override { callback(std::move(completion)); }
};
struct OperationJob final : Job {
    MetadataExecutor::Operation operation;
    Completion<void> callback;
    TaskCompletion<void> completion;
    ErrorCode exceptionCode;
    OperationJob(MetadataExecutor::Operation work, Completion<void> cb, ErrorCode error, std::shared_ptr<TaskControl> task)
        : Job(std::move(task)), operation(std::move(work)), callback(std::move(cb)),
          completion{control->id, Result<void>(ErrorCode::InternalError)}, exceptionCode(error) {}
    void execute(MetadataWorkerResources& resources) override { completion.outcome = operation(resources); }
    void failed() noexcept override { completion.outcome = Result<void>(exceptionCode); }
    void cancelled() noexcept override { completion.outcome = CancelledBeforeStart{}; }
    void deliver() override { callback(std::move(completion)); }
};

std::size_t capacity(MetadataExecutorOptions options) {
    if (options.maxPending == 0 || options.maxPending == std::numeric_limits<std::size_t>::max() ||
        options.drainBatch == 0 || options.drainBatch > 32) {
        throw std::invalid_argument("Invalid metadata executor capacity");
    }
    return options.maxPending;
}

static_assert(std::is_nothrow_move_assignable_v<CreateDirectoryCompletion>);
static_assert(std::is_nothrow_move_constructible_v<CreateDirectoryCompletion>);

}  // namespace

struct MetadataExecutor::Impl {
    muduox::net::EventLoop& loop;
    const MetadataExecutorOptions options;
    mutable std::mutex mutex;
    std::condition_variable changed;
    // 两个 ring 均在启动前分配；完成路径不再扩容，也不竞争待执行队列。
    std::vector<std::unique_ptr<Job>> pending;
    std::vector<std::unique_ptr<Job>> completed;
    std::size_t pendingHead = 0, pendingCount = 0;
    std::size_t completedHead = 0, completedCount = 0;
    std::size_t outstanding = 0;
    bool running = false;
    bool stopping = false;
    bool startupAvailable = false;
    bool startupDelivered = false;
    bool workerFinished = false;
    Result<void> startupResult;
    MetadataExecutorState state = MetadataExecutorState::Created;  // 仅 loop 使用。
    std::uint64_t nextId = 1;
    std::size_t taskExceptions = 0;
    std::size_t callbackExceptions = 0;  // 仅 loop 使用。
    Started onStarted;
    Stopped onStopped;
    std::promise<void> exitPromise;
    std::future<void> exited = exitPromise.get_future();
    std::thread worker;
    CompletionPump pump;

    Impl(muduox::net::EventLoop& owner, MetadataExecutorOptions limits)
        : loop(owner), options(limits), pending(capacity(limits)), completed(limits.maxPending + 1),
          pump(owner, [this] { return drain(); }, [this] { finishIfIdle(); }) {}

    void publishStartup(Result<void> result) noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            startupResult = result;
            startupAvailable = true;
            if (!result) stopping = true;
        }
        pump.notify();
    }

    void work(Factory factory) noexcept {
        try {
            // 登记在真正线程退出时就绪；loop 只在 future 就绪后 join。
            exitPromise.set_value_at_thread_exit();
        } catch (...) {
            // 尚未打开资源或接受业务；注册失败仍通知启动失败和清理完成。
            factory = {};
            publishStartup(ErrorCode::InternalError);
            finishWorker();
            // 登记 thread-exit 的分配失败时没有任何业务资源；最后一次通知后才发布就绪。
            exitPromise.set_value();
            return;
        }
        {
            std::optional<MetadataWorkerResources> resources;
            try {
                auto opened = factory();
                factory = {};  // 工厂闭包也在 worker 释放，先于退出通知。
                if (!opened) publishStartup(opened.error());
                else {
                    resources.emplace(std::move(opened).value());
                    if (!resources->metadata || !resources->clock || !resources->nodeIds) publishStartup(ErrorCode::InternalError);
                    else {
                        publishStartup({});
                        runJobs(*resources);
                    }
                }
            } catch (...) {
                // 工厂／资源构造阶段尚未接受任务，和执行阶段的提交不确定分开。
                factory = {};
                publishStartup(ErrorCode::InternalError);
            }
            // metadata、clock、nodeIds 及临时 factory 返回值均在此线程释放。
        }
        finishWorker();
    }

    void runJobs(MetadataWorkerResources& resources) noexcept {
        for (;;) {
            std::unique_ptr<Job> job;
            bool execute;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this] { return stopping || pendingCount != 0; });
                if (pendingCount == 0) return;
                job = std::move(pending[pendingHead]);
                pendingHead = (pendingHead + 1) % pending.size();
                --pendingCount;
                auto expected = TaskState::Queued;
                execute = job->control->state.compare_exchange_strong(expected, TaskState::Running);
                running = execute;
            }
            if (execute) {
                try {
                    job->execute(resources);
                } catch (...) {
                    // execute 抛异常时不得猜测是否已提交；保留已生成的诊断 ID，不重试。
                    job->failed();
                    std::lock_guard<std::mutex> lock(mutex);
                    ++taskExceptions;
                }
            } else job->cancelled();
            job->control->state.store(TaskState::Terminal);
            {
                std::lock_guard<std::mutex> lock(mutex);
                running = false;
                completed[(completedHead + completedCount) % completed.size()] = std::move(job);
                ++completedCount;
            }
            pump.notify();
        }
    }

    void finishWorker() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            workerFinished = true;
        }
        internal::shutdownTrace("metadata", "worker_final_notify", &pump);
        pump.notify();
        internal::shutdownTrace("metadata", "worker_returning", &pump);
    }

    bool drain() noexcept {
        loop.assertInLoopThread();
        bool report;
        Result<void> result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            report = startupAvailable && !startupDelivered;
            if (report) {
                startupDelivered = true;
                result = startupResult;
            }
        }
        if (report) {
            if (!result) state = MetadataExecutorState::Stopping;
            else if (state == MetadataExecutorState::Starting) state = MetadataExecutorState::Ready;
            auto callback = std::move(onStarted);
            try { if (callback) callback(result); }
            catch (...) { ++callbackExceptions; }
        }
        for (std::size_t i = 0; i < options.drainBatch; ++i) {
            std::unique_ptr<Job> job;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (completedCount == 0) break;
                job = std::move(completed[completedHead]);
                completedHead = (completedHead + 1) % completed.size();
                --completedCount;
            }
            try { job->deliver(); }
            catch (...) { ++callbackExceptions; }
            // 回调及其闭包均在 loop 销毁；直到回调结束才归还总名额。
            job.reset();
            {
                std::lock_guard<std::mutex> lock(mutex);
                --outstanding;
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        return completedCount != 0;
    }

    void finishIfIdle() noexcept {
        if (state != MetadataExecutorState::Stopping || !pump.idle()) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!workerFinished || outstanding != 0 || !startupDelivered) return;
        }
        if (exited.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            internal::shutdownTrace("metadata", "exit_future_pending", &pump);
            return;
        }
        internal::shutdownTrace("metadata", "exit_future_ready", &pump);
        // future 在真实线程退出时就绪；无连接、任务或后续 worker 投递存活。
        if (worker.joinable()) worker.join();
        if (!pump.idle()) return;
        state = MetadataExecutorState::Stopped;
        internal::shutdownTrace("metadata", "stopped", &pump);
        auto callback = std::move(onStopped);
        try { if (callback) callback(); }
        catch (...) { ++callbackExceptions; }
    }

    void stop() {
        loop.assertInLoopThread();
        if (state == MetadataExecutorState::Stopped || state == MetadataExecutorState::Stopping) return;
        if (state == MetadataExecutorState::Created) {
            state = MetadataExecutorState::Stopped;
            return;
        }
        internal::shutdownTrace("metadata", "stop_requested", &pump);
        state = MetadataExecutorState::Stopping;
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            for (std::size_t i = 0; i < pendingCount; ++i) {
                auto expected = TaskState::Queued;
                pending[(pendingHead + i) % pending.size()]->control->state.compare_exchange_strong(expected, TaskState::Cancelled);
            }
        }
        changed.notify_one();
        pump.notify();
    }
};

MetadataExecutor::MetadataExecutor(muduox::net::EventLoop& loop, MetadataExecutorOptions options)
    : impl_(std::make_unique<Impl>(loop, options)) {}

MetadataExecutor::~MetadataExecutor() {
    impl_->loop.assertInLoopThread();
    if (impl_->worker.joinable() || impl_->outstanding != 0) std::terminate();
}

Result<void> MetadataExecutor::start(Factory factory, Started started, Stopped stopped) {
    auto& self = *impl_;
    self.loop.assertInLoopThread();
    if (!factory || self.state != MetadataExecutorState::Created) return ErrorCode::InvalidArgument;
    self.onStarted = std::move(started);
    self.onStopped = std::move(stopped);
    self.state = MetadataExecutorState::Starting;
    try {
        self.worker = std::thread([&self, factory = std::move(factory)]() mutable { self.work(std::move(factory)); });
    } catch (...) {
        self.state = MetadataExecutorState::Stopped;
        self.onStarted = {};
        self.onStopped = {};
        return ErrorCode::InternalError;
    }
    return {};
}

TaskSubmission MetadataExecutor::trySubmit(RequestContext context, CreateDirectoryCommand command,
                                                       CreateDirectoryCallback completion) {
    auto& self = *impl_;
    self.loop.assertInLoopThread();
    if (self.state != MetadataExecutorState::Ready) return SubmitError::Stopping;
    if (!completion || self.nextId == std::numeric_limits<std::uint64_t>::max()) return SubmitError::InternalError;
    try {
        std::lock_guard<std::mutex> lock(self.mutex);
        if (self.pendingCount == self.pending.size() || self.outstanding == self.completed.size()) return SubmitError::QueueFull;
        auto control = std::make_shared<TaskControl>(self.nextId);
        auto handle = std::make_unique<Handle>(control);
        auto job = std::make_unique<DirectoryJob>(std::move(context), std::move(command), std::move(completion), control);
        self.pending[(self.pendingHead + self.pendingCount) % self.pending.size()] = std::move(job);
        ++self.pendingCount;
        ++self.outstanding;
        ++self.nextId;
        self.changed.notify_one();
        return AcceptedTask{ std::move(handle) };
    } catch (...) {
        return SubmitError::InternalError;
    }
}

TaskSubmission MetadataExecutor::submitOperation(Operation operation, Completion<void> completion, ErrorCode exceptionCode) {
    auto& self = *impl_;
    self.loop.assertInLoopThread();
    if (self.state != MetadataExecutorState::Ready) return SubmitError::Stopping;
    if (!operation || !completion || self.nextId == std::numeric_limits<std::uint64_t>::max()) return SubmitError::InternalError;
    try {
        std::lock_guard<std::mutex> lock(self.mutex);
        if (self.pendingCount == self.pending.size() || self.outstanding == self.completed.size()) return SubmitError::QueueFull;
        auto control = std::make_shared<TaskControl>(self.nextId);
        auto handle = std::make_unique<Handle>(control);
        auto job = std::make_unique<OperationJob>(std::move(operation), std::move(completion), exceptionCode, control);
        self.pending[(self.pendingHead + self.pendingCount) % self.pending.size()] = std::move(job);
        ++self.pendingCount; ++self.outstanding; ++self.nextId;
        self.changed.notify_one();
        return AcceptedTask{std::move(handle)};
    } catch (...) { return SubmitError::InternalError; }
}

void MetadataExecutor::requestStop() { impl_->stop(); }

MetadataExecutorSnapshot MetadataExecutor::snapshot() const {
    const auto& self = *impl_;
    self.loop.assertInLoopThread();
    std::lock_guard<std::mutex> lock(self.mutex);
    return { self.state, self.pendingCount, self.completedCount, self.outstanding, self.running,
             self.taskExceptions, self.callbackExceptions, self.pump.postFailures() };
}

}  // namespace anof::runtime
