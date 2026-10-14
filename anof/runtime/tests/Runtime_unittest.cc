#define BOOST_TEST_MODULE AnofRuntime
#include <boost/test/unit_test.hpp>

#include <anof/runtime/AnofRuntime.h>
#include <anof/runtime/CompletionPump.h>

#include <muduox/net/EventLoop.h>

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace anof;
using namespace anof::runtime;

namespace {

// 只为测试协调确定的先后关系；超时自动释放 worker，避免失败测试永久卡住。
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    bool expired = false;
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(2), [this] { return released; })) expired = true;
    }
    bool awaitEntry() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(2), [this] { return entered; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }
};

bool drive(muduox::net::EventLoop& loop, const std::function<bool()>& done) {
    if (done()) return true;
    bool expired = false;
    auto check = loop.runEvery(0.001, [&] { if (done()) loop.quit(); });
    auto deadline = loop.runAfter(3, [&] { expired = true; loop.quit(); });
    loop.loop();
    loop.cancel(check);
    loop.cancel(deadline);
    return !expired && done();
}

struct Tracking {
    Gate first;
    Gate second;
    Gate startup;
    Gate threadExit;
    std::atomic<int> calls{ 0 };
    std::atomic<int> destroyed{ 0 };
    std::atomic<bool> wrongThread{ false };
    bool blockFirst = false;
    bool blockSecond = false;
    int secondCall = 2;
    bool blockStartup = false;
    bool blockThreadExit = false;
    bool throwFirst = false;
    std::thread::id worker;
};

struct Owned {
    std::shared_ptr<Tracking> tracking;
    explicit Owned(std::shared_ptr<Tracking> data) : tracking(std::move(data)) {}
    void checkThread() const {
        if (tracking->worker != std::this_thread::get_id()) tracking->wrongThread = true;
    }
    ~Owned() { checkThread(); ++tracking->destroyed; }
};

struct Store final : MetadataStore, Owned {
    using Owned::Owned;
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override {
        BOOST_FAIL("Unexpected read in write-only executor fixture");
        return ErrorCode::InternalError;
    }
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override {
        checkThread();
        const int call = ++tracking->calls;
        if (call == 1 && tracking->blockFirst) tracking->first.wait();
        if (call == tracking->secondCall && tracking->blockSecond) tracking->second.wait();
        if (call == 1 && tracking->throwFirst) throw std::runtime_error("Injected execution failure");
        return ErrorCode::Retryable;
    }
};

struct TestClock final : Clock, Owned {
    using Owned::Owned;
    UtcTime now() const override { checkThread(); return UtcTime{}; }
};

struct TestIds final : NodeIdGenerator, Owned {
    using Owned::Owned;
    Result<NodeId> next() override {
        checkThread();
        return NodeId::parse("30000000-0000-4000-8000-000000000001");
    }
};

MetadataExecutor::Factory factory(const std::shared_ptr<Tracking>& data) {
    return [data]() -> Result<MetadataWorkerResources> {
        data->worker = std::this_thread::get_id();
        if (data->blockThreadExit) {
            struct ThreadExitGate {
                std::shared_ptr<Tracking> tracking;
                ~ThreadExitGate() { tracking->threadExit.wait(); }
            };
            thread_local std::unique_ptr<ThreadExitGate> exitGate;
            exitGate = std::make_unique<ThreadExitGate>();
            exitGate->tracking = data;
        }
        if (data->blockStartup) data->startup.wait();
        return MetadataWorkerResources{ std::make_unique<Store>(data), std::make_unique<TestClock>(data), std::make_unique<TestIds>(data) };
    };
}

RequestContext context() {
    return { { SessionDigest(std::array<std::uint8_t, 32>{}), CredentialMode::Bearer }, RequestId::parse("50000000-0000-4000-8000-000000000001").value() };
}
CreateDirectoryCommand command() {
    return { SpaceId::parse("10000000-0000-4000-8000-000000000001").value(),
             NodeId::parse("20000000-0000-4000-8000-000000000001").value(), "directory" };
}

struct Fixture {
    muduox::net::EventLoop loop;
    std::shared_ptr<Tracking> tracking = std::make_shared<Tracking>();
    MetadataExecutor executor;
    explicit Fixture(MetadataExecutorOptions options = {}) : executor(loop, options) {}
    ~Fixture() {
        tracking->first.release();
        tracking->second.release();
        tracking->startup.release();
        tracking->threadExit.release();
        executor.requestStop();
        if (!drive(loop, [this] { return executor.snapshot().state == MetadataExecutorState::Stopped; })) std::terminate();
    }
    void start() {
        BOOST_REQUIRE(static_cast<bool>(executor.start(factory(tracking))));
        BOOST_REQUIRE(drive(loop, [this] { return executor.snapshot().state == MetadataExecutorState::Ready; }));
    }
};

std::unique_ptr<TaskHandle> accepted(TaskSubmission submission) {
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(submission));
    auto handle = std::move(std::get<AcceptedTask>(submission).handle);
    BOOST_REQUIRE(handle);
    return handle;
}
void rejected(const TaskSubmission& submission, SubmitError expected) {
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(submission));
    BOOST_CHECK(std::get<SubmitError>(submission) == expected);
}

}  // namespace

BOOST_AUTO_TEST_CASE(invalid_capacity_and_lifecycle_rejections) {
    muduox::net::EventLoop loop;
    BOOST_CHECK_THROW((MetadataExecutor(loop, { 0, 1 })), std::invalid_argument);
    BOOST_CHECK_THROW((MetadataExecutor(loop, { 1, 0 })), std::invalid_argument);
    BOOST_CHECK_THROW((MetadataExecutor(loop, { 1, 33 })), std::invalid_argument);
    BOOST_CHECK_THROW((MetadataExecutor(loop, { std::numeric_limits<std::size_t>::max(), 1 })), std::invalid_argument);
    MetadataExecutor executor(loop);
    bool called = false;
    rejected(executor.trySubmit(context(), command(), [&](auto) { called = true; }), SubmitError::Stopping);
    BOOST_CHECK(!executor.start({}));
    executor.requestStop();
    BOOST_CHECK(!executor.start([]() -> Result<MetadataWorkerResources> { return ErrorCode::InternalError; }));
    BOOST_CHECK(!called);
}

BOOST_AUTO_TEST_CASE(deferred_owner_loop_delivery_and_worker_resource_lifetime) {
    std::unique_ptr<TaskHandle> handle;
    auto data = std::make_shared<Tracking>();
    {
        Fixture f;
        f.tracking = data;
        int started = 0, stopped = 0, completed = 0;
        const auto owner = std::this_thread::get_id();
        BOOST_REQUIRE(f.executor.start(factory(data), [&](Result<void> r) {
            BOOST_CHECK(r.hasValue()); BOOST_CHECK(std::this_thread::get_id() == owner); ++started;
        }, [&] { ++stopped; }));
        BOOST_CHECK_EQUAL(started, 0);
        BOOST_REQUIRE(drive(f.loop, [&] { return started == 1; }));
        BOOST_CHECK(data->worker != owner);
        rejected(f.executor.trySubmit(context(), command(), {}), SubmitError::InternalError);
        handle = accepted(f.executor.trySubmit(context(), command(), [&](CreateDirectoryCompletion result) {
            BOOST_CHECK(std::this_thread::get_id() == owner);
            BOOST_CHECK_EQUAL(result.taskId, handle->id());
            BOOST_CHECK(std::get<Result<CreateDirectoryResult>>(result.outcome).error().code == ErrorCode::Retryable);
            BOOST_REQUIRE(result.attempt);
            BOOST_CHECK(result.attempt->requestId == context().requestId);
            ++completed;
        }));
        BOOST_CHECK_EQUAL(completed, 0);
        BOOST_REQUIRE(drive(f.loop, [&] { return completed == 1; }));
        BOOST_CHECK_EQUAL(f.executor.snapshot().outstanding, 0U);
        BOOST_CHECK(handle->cancelBeforeStart() == CancelBeforeStartResult::AlreadyTerminal);
        f.executor.requestStop();
        f.executor.requestStop();
        BOOST_REQUIRE(drive(f.loop, [&] { return stopped == 1; }));
        BOOST_CHECK_EQUAL(data->destroyed.load(), 3);
    }
    BOOST_CHECK(handle->cancelBeforeStart() == CancelBeforeStartResult::AlreadyTerminal);
    BOOST_CHECK(!data->wrongThread.load());
}

BOOST_AUTO_TEST_CASE(pending_and_unretired_limits_are_independent) {
    Fixture f({ 1, 1 });
    f.tracking->blockFirst = true;
    f.tracking->blockSecond = true;
    f.start();
    int delivered = 0;
    auto first = accepted(f.executor.trySubmit(context(), command(), [&](auto) {
        ++delivered;
        // 第一回调尚未返回，总名额仍被它占用，不能在此重入超卖。
        rejected(f.executor.trySubmit(context(), command(), [](auto) {}), SubmitError::QueueFull);
    }));
    BOOST_REQUIRE(f.tracking->first.awaitEntry());
    auto second = accepted(f.executor.trySubmit(context(), command(), [&](auto) { ++delivered; }));
    rejected(f.executor.trySubmit(context(), command(), [](auto) {}), SubmitError::QueueFull);
    f.tracking->first.release();
    BOOST_REQUIRE(f.tracking->second.awaitEntry());
    auto snapshot = f.executor.snapshot();
    BOOST_CHECK_EQUAL(snapshot.pending, 0U);
    BOOST_CHECK_EQUAL(snapshot.completed, 1U);
    BOOST_CHECK_EQUAL(snapshot.outstanding, 2U);
    rejected(f.executor.trySubmit(context(), command(), [](auto) {}), SubmitError::QueueFull);
    BOOST_REQUIRE(drive(f.loop, [&] { return delivered == 1; }));
    auto third = accepted(f.executor.trySubmit(context(), command(), [&](auto result) {
        BOOST_CHECK(std::holds_alternative<CancelledBeforeStart>(result.outcome)); ++delivered;
    }));
    BOOST_CHECK(third->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    BOOST_CHECK(second->cancelBeforeStart() == CancelBeforeStartResult::AlreadyRunning);
    f.executor.requestStop();
    f.tracking->second.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.executor.snapshot().state == MetadataExecutorState::Stopped; }));
    BOOST_CHECK_EQUAL(delivered, 3);
    BOOST_CHECK_EQUAL(f.tracking->calls.load(), 2);
    BOOST_CHECK_EQUAL(f.executor.snapshot().outstanding, 0U);
}

BOOST_AUTO_TEST_CASE(cancel_before_start_wins_and_handle_destruction_does_not_cancel) {
    Fixture f({ 2, 32 });
    f.tracking->blockFirst = true;
    f.start();
    int cancelled = 0, executed = 0;
    accepted(f.executor.trySubmit(context(), command(), [&](auto) { ++executed; })).reset();
    BOOST_REQUIRE(f.tracking->first.awaitEntry());
    auto cancelledHandle = accepted(f.executor.trySubmit(context(), command(), [&](auto result) {
        BOOST_CHECK(std::holds_alternative<CancelledBeforeStart>(result.outcome));
        BOOST_CHECK(!result.attempt); ++cancelled;
    }));
    BOOST_CHECK(cancelledHandle->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    BOOST_CHECK(cancelledHandle->cancelBeforeStart() == CancelBeforeStartResult::AlreadyTerminal);
    accepted(f.executor.trySubmit(context(), command(), [&](auto result) {
        BOOST_CHECK(std::holds_alternative<Result<CreateDirectoryResult>>(result.outcome)); ++executed;
    })).reset();
    f.tracking->first.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return cancelled + executed == 3; }));
    BOOST_CHECK_EQUAL(executed, 2);
    BOOST_CHECK_EQUAL(f.tracking->calls.load(), 2);
}

BOOST_AUTO_TEST_CASE(stop_cancels_waiting_tasks_but_preserves_running_and_cleanup) {
    Fixture f({ 2, 1 });
    f.tracking->blockFirst = true;
    f.start();
    int callbacks = 0;
    auto running = accepted(f.executor.trySubmit(context(), command(), [&](auto result) {
        BOOST_CHECK(std::holds_alternative<Result<CreateDirectoryResult>>(result.outcome)); ++callbacks;
    }));
    BOOST_REQUIRE(f.tracking->first.awaitEntry());
    for (int i = 0; i < 2; ++i) accepted(f.executor.trySubmit(context(), command(), [&](auto result) {
        BOOST_CHECK(std::holds_alternative<CancelledBeforeStart>(result.outcome)); ++callbacks;
    }));
    f.executor.requestStop();
    BOOST_CHECK(f.executor.snapshot().state == MetadataExecutorState::Stopping);
    BOOST_CHECK_EQUAL(f.tracking->destroyed.load(), 0);
    rejected(f.executor.trySubmit(context(), command(), [](auto) {}), SubmitError::Stopping);
    // loop 上的其他回调仍可执行；不得因 stop 等待被 gate 阻塞的 worker。
    bool responsive = false;
    f.loop.queueInLoop([&] { responsive = true; });
    BOOST_REQUIRE(drive(f.loop, [&] { return responsive; }));
    BOOST_CHECK_EQUAL(callbacks, 0);
    f.tracking->first.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.executor.snapshot().state == MetadataExecutorState::Stopped; }));
    BOOST_CHECK_EQUAL(callbacks, 3);
    BOOST_CHECK_EQUAL(f.tracking->destroyed.load(), 3);
}

BOOST_AUTO_TEST_CASE(execute_and_callback_exceptions_do_not_lose_capacity_or_worker) {
    Fixture f({ 2, 1 });
    f.tracking->throwFirst = true;
    f.start();
    int callbacks = 0;
    accepted(f.executor.trySubmit(context(), command(), [&](auto result) {
        BOOST_CHECK(std::get<Result<CreateDirectoryResult>>(result.outcome).error().code == ErrorCode::CommitOutcomeUnknown);
        BOOST_CHECK(result.attempt.has_value()); ++callbacks;
        throw std::runtime_error("Injected callback failure");
    }));
    BOOST_REQUIRE(drive(f.loop, [&] { return callbacks == 1; }));
    accepted(f.executor.trySubmit(context(), command(), [&](auto result) {
        BOOST_CHECK(std::get<Result<CreateDirectoryResult>>(result.outcome).error().code == ErrorCode::Retryable); ++callbacks;
    }));
    BOOST_REQUIRE(drive(f.loop, [&] { return callbacks == 2; }));
    const auto snapshot = f.executor.snapshot();
    BOOST_CHECK_EQUAL(snapshot.taskExceptions, 1U);
    BOOST_CHECK_EQUAL(snapshot.callbackExceptions, 1U);
    BOOST_CHECK_EQUAL(snapshot.outstanding, 0U);
    BOOST_CHECK_EQUAL(f.tracking->calls.load(), 2);
}

BOOST_AUTO_TEST_CASE(failed_factory_and_invalid_resources_complete_startup_cleanup) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f;
        int starts = 0, stops = 0;
        BOOST_REQUIRE(f.executor.start([mode]() -> Result<MetadataWorkerResources> {
            if (mode == 0) return ErrorCode::StorageFull;
            if (mode == 1) throw std::runtime_error("Injected startup failure");
            return MetadataWorkerResources{};
        }, [&](Result<void> result) {
            BOOST_CHECK(!result); ++starts;
            throw std::runtime_error("Injected startup callback failure");
        }, [&] { ++stops; throw std::runtime_error("Injected stopped callback failure"); }));
        BOOST_REQUIRE(drive(f.loop, [&] { return f.executor.snapshot().state == MetadataExecutorState::Stopped; }));
        BOOST_CHECK_EQUAL(starts, 1);
        BOOST_CHECK_EQUAL(stops, 1);
        BOOST_CHECK_EQUAL(f.executor.snapshot().callbackExceptions, 2U);
        BOOST_CHECK(!f.executor.start(factory(f.tracking)));
    }
}

BOOST_AUTO_TEST_CASE(stop_during_startup_never_reopens_admission) {
    Fixture f;
    f.tracking->blockStartup = true;
    int starts = 0;
    BOOST_REQUIRE(f.executor.start(factory(f.tracking), [&](Result<void> result) {
        BOOST_CHECK(result.hasValue()); ++starts;
        BOOST_CHECK(f.executor.snapshot().state == MetadataExecutorState::Stopping);
        rejected(f.executor.trySubmit(context(), command(), [](auto) {}), SubmitError::Stopping);
    }));
    BOOST_REQUIRE(f.tracking->startup.awaitEntry());
    f.executor.requestStop();
    f.tracking->startup.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.executor.snapshot().state == MetadataExecutorState::Stopped; }));
    BOOST_CHECK_EQUAL(starts, 1);
    BOOST_CHECK_EQUAL(f.tracking->destroyed.load(), 3);
}

BOOST_AUTO_TEST_CASE(completion_closure_is_destroyed_on_loop_and_can_stop_reentrantly) {
    Fixture f;
    f.start();
    std::thread::id destroyedOn;
    auto witness = std::shared_ptr<int>(new int(0), [&](int* value) { destroyedOn = std::this_thread::get_id(); delete value; });
    accepted(f.executor.trySubmit(context(), command(), [&, witness](auto) { f.executor.requestStop(); }));
    witness.reset();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.executor.snapshot().state == MetadataExecutorState::Stopped; }));
    BOOST_CHECK(destroyedOn == std::this_thread::get_id());
}

BOOST_AUTO_TEST_CASE(pump_coalesces_notifications_and_yields_between_batches) {
    muduox::net::EventLoop loop;
    int calls = 0, idle = 0;
    std::vector<int64_t> iterations;
    CompletionPump pump(loop, [&] {
        iterations.push_back(loop.iteration());
        return ++calls < 3;
    }, [&] { ++idle; });
    for (int i = 0; i < 16; ++i) pump.notify();
    BOOST_CHECK_EQUAL(loop.queueSize(), 1U);
    BOOST_REQUIRE(drive(loop, [&] { return pump.idle() && calls == 3; }));
    BOOST_CHECK_EQUAL(calls, 3);
    BOOST_CHECK(iterations[0] < iterations[1]);
    BOOST_CHECK(iterations[1] < iterations[2]);
    BOOST_CHECK_GE(idle, 1);
}

BOOST_AUTO_TEST_CASE(pump_notify_during_drain_and_after_idle_cannot_lose_wakeup) {
    muduox::net::EventLoop loop;
    int calls = 0;
    CompletionPump* access = nullptr;
    CompletionPump pump(loop, [&] {
        if (++calls == 1) {
            std::thread producer([&] { access->notify(); });
            producer.join();  // 测试限定通知期间无业务或 loop 依赖。
        }
        return false;
    }, [] {});
    access = &pump;
    pump.notify();
    BOOST_REQUIRE(drive(loop, [&] { return calls == 2 && pump.idle(); }));
    pump.notify();
    BOOST_REQUIRE(drive(loop, [&] { return calls == 3 && pump.idle(); }));
}

BOOST_AUTO_TEST_CASE(pump_preserves_work_when_post_throws) {
    muduox::net::EventLoop loop;
    int posts = 0, delivered = 0;
    CompletionPump pump(loop, [&] { return ++delivered < 3; }, [] {}, [&](auto callback) {
        if (++posts <= 2) throw std::bad_alloc();
        loop.queueInLoop(std::move(callback));
    });
    pump.notify();
    pump.notify();
    BOOST_CHECK(!pump.idle());
    BOOST_REQUIRE(drive(loop, [&] { return delivered == 3 && pump.idle(); }));
    BOOST_CHECK_EQUAL(pump.postFailures(), 2U);
    BOOST_CHECK_EQUAL(posts, 3);
}

BOOST_AUTO_TEST_CASE(runtime_waits_for_both_http_and_metadata_barriers) {
    for (bool httpFirst : { false, true }) {
        muduox::net::EventLoop loop;
        auto data = std::make_shared<Tracking>();
        AnofRuntime runtime(loop, { 1, 1 });
        int initialized = 0, stopped = 0;
        BOOST_CHECK(!runtime.ready());
        BOOST_CHECK(!runtime.markHttpStarted());
        BOOST_REQUIRE(runtime.start(factory(data), [&](Result<void> result) {
            BOOST_CHECK(result.hasValue()); ++initialized;
        }, [&] { ++stopped; }));
        BOOST_REQUIRE(drive(loop, [&] { return initialized == 1; }));
        BOOST_CHECK(!runtime.ready());
        BOOST_REQUIRE(runtime.markHttpStarted());
        BOOST_CHECK(runtime.ready());
        BOOST_CHECK(!runtime.markHttpStopped());
        runtime.requestStop();
        BOOST_CHECK(!runtime.ready());
        if (httpFirst) BOOST_CHECK(runtime.markHttpStopped());
        BOOST_REQUIRE(drive(loop, [&] { return runtime.metadataSnapshot().state == MetadataExecutorState::Stopped; }));
        if (!httpFirst) {
            BOOST_CHECK_EQUAL(stopped, 0);
            BOOST_CHECK(runtime.state() == RuntimeState::Stopping);
            BOOST_CHECK(runtime.markHttpStopped());
        }
        BOOST_CHECK(runtime.state() == RuntimeState::Stopped);
        BOOST_CHECK_EQUAL(stopped, 1);
        BOOST_CHECK(!runtime.markHttpStopped());
        BOOST_CHECK(!runtime.markHttpStarted());
        BOOST_CHECK(!runtime.start(factory(data)));
    }
}

BOOST_AUTO_TEST_CASE(runtime_failed_start_and_stop_without_http_are_terminal) {
    muduox::net::EventLoop loop;
    AnofRuntime runtime(loop);
    int stopped = 0;
    BOOST_REQUIRE(runtime.start([]() -> Result<MetadataWorkerResources> { return ErrorCode::InternalError; }, {}, [&] { ++stopped; }));
    BOOST_REQUIRE(drive(loop, [&] { return runtime.state() == RuntimeState::Stopped; }));
    BOOST_CHECK_EQUAL(stopped, 1);
    BOOST_CHECK(!runtime.ready());
    AnofRuntime unused(loop);
    unused.requestStop();
    BOOST_CHECK(unused.state() == RuntimeState::Stopped);
    AnofRuntime rejectedStart(loop);
    BOOST_CHECK(!rejectedStart.start({}));
    BOOST_CHECK(rejectedStart.state() == RuntimeState::Stopped);
}

BOOST_AUTO_TEST_CASE(default_drain_batch_is_bounded_to_32_completions) {
    Fixture f({ 33, 32 });
    f.tracking->blockFirst = true;
    f.tracking->blockSecond = true;
    f.tracking->secondCall = 34;
    f.start();
    std::vector<int64_t> iterations;
    auto callback = [&](auto) { iterations.push_back(f.loop.iteration()); };
    accepted(f.executor.trySubmit(context(), command(), callback));
    BOOST_REQUIRE(f.tracking->first.awaitEntry());
    for (int i = 0; i < 33; ++i) accepted(f.executor.trySubmit(context(), command(), callback));
    f.tracking->first.release();
    BOOST_REQUIRE(f.tracking->second.awaitEntry());
    BOOST_CHECK_EQUAL(f.executor.snapshot().completed, 33U);
    BOOST_CHECK_LE(f.loop.queueSize(), 1U);
    BOOST_REQUIRE(drive(f.loop, [&] { return iterations.size() == 33; }));
    BOOST_CHECK_EQUAL(iterations[0], iterations[31]);
    BOOST_CHECK(iterations[31] < iterations[32]);
    f.tracking->second.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return iterations.size() == 34; }));
}

BOOST_AUTO_TEST_CASE(cancel_and_worker_start_race_has_one_consistent_terminal_result) {
    // 四次有界竞争只验证两种合法终态，不依靠次数或时序保证某一方取胜。
    for (int trial = 0; trial < 4; ++trial) {
        Fixture f({ 1, 1 });
        f.tracking->blockFirst = true;
        f.start();
        int delivered = 0;
        bool cancelled = false;
        accepted(f.executor.trySubmit(context(), command(), [&](auto) { ++delivered; }));
        BOOST_REQUIRE(f.tracking->first.awaitEntry());
        auto handle = accepted(f.executor.trySubmit(context(), command(), [&](auto result) {
            cancelled = std::holds_alternative<CancelledBeforeStart>(result.outcome); ++delivered;
        }));
        CancelBeforeStartResult won = CancelBeforeStartResult::AlreadyTerminal;
        std::thread contender([&] { won = handle->cancelBeforeStart(); });
        f.tracking->first.release();
        contender.join();
        BOOST_REQUIRE(drive(f.loop, [&] { return delivered == 2; }));
        BOOST_CHECK_EQUAL(cancelled, won == CancelBeforeStartResult::Cancelled);
        BOOST_CHECK_EQUAL(f.tracking->calls.load(), cancelled ? 1 : 2);
        BOOST_CHECK_EQUAL(f.executor.snapshot().outstanding, 0U);
    }
}

BOOST_AUTO_TEST_CASE(late_result_can_discard_expired_request_and_still_retire) {
    Fixture f;
    f.tracking->blockFirst = true;
    f.start();
    auto request = std::make_shared<int>(0);
    std::weak_ptr<int> weak = request;
    int dropped = 0;
    accepted(f.executor.trySubmit(context(), command(), [weak, &dropped](auto) {
        if (auto state = weak.lock()) ++*state;
        else ++dropped;
    }));
    BOOST_REQUIRE(f.tracking->first.awaitEntry());
    request.reset();
    f.tracking->first.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return dropped == 1; }));
    BOOST_CHECK_EQUAL(f.executor.snapshot().outstanding, 0U);
}

BOOST_AUTO_TEST_CASE(runtime_stop_notification_failure_does_not_reopen_state) {
    muduox::net::EventLoop loop;
    AnofRuntime runtime(loop);
    BOOST_REQUIRE(runtime.start([]() -> Result<MetadataWorkerResources> { return ErrorCode::InternalError; }, {}, [] {
        throw std::runtime_error("Injected runtime notification failure");
    }));
    BOOST_REQUIRE(drive(loop, [&] { return runtime.state() == RuntimeState::Stopped; }));
    BOOST_CHECK_EQUAL(runtime.notificationExceptions(), 1U);
}

BOOST_AUTO_TEST_CASE(stop_does_not_join_until_thread_local_cleanup_has_exited) {
    Fixture f;
    f.tracking->blockThreadExit = true;
    f.start();
    f.executor.requestStop();
    BOOST_REQUIRE(f.tracking->threadExit.awaitEntry());
    // 业务资源已经关闭、完成通知已发布，但线程自己的清理尚未结束。
    BOOST_CHECK_EQUAL(f.tracking->destroyed.load(), 3);
    bool responsive = false;
    f.loop.queueInLoop([&] { responsive = true; });
    BOOST_REQUIRE(drive(f.loop, [&] { return responsive; }));
    BOOST_CHECK(f.executor.snapshot().state == MetadataExecutorState::Stopping);
    BOOST_CHECK_EQUAL(f.executor.snapshot().outstanding, 0U);
    f.tracking->threadExit.release();
    BOOST_REQUIRE(drive(f.loop, [&] { return f.executor.snapshot().state == MetadataExecutorState::Stopped; }));
}
