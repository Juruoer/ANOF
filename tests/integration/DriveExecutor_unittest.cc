#define BOOST_TEST_MODULE DriveExecutorIntegration
#include <tests/integration/DriveFixture.h>
#include <tests/support/EventLoopWait.h>
#include <anof/runtime/DriveExecutor.h>
#include <muduox/net/EventLoop.h>
#include <atomic>
#include <future>
using namespace anof;
using namespace anof::runtime;
using namespace anof::testing::drive;
namespace {
struct Runtime : Fixture {
    muduox::net::EventLoop loop;
    MetadataExecutor metadata{loop, {4, 4}};
    DriveExecutor drive{loop, metadata, 2};
    void start(bool brokenIds = false) {
        resources = {};
        bool ready = false;
        BOOST_REQUIRE(metadata.start([this, brokenIds]() -> Result<MetadataWorkerResources> {
            auto r = makeInstanceMetadataResources(root / ".anof/metadata.sqlite3");
            if (!r) return r.error();
            if (brokenIds) {
                struct ThrowingIds : DriveIdGenerator { Result<TrashId> trashId() override { throw std::runtime_error("injected id failure"); } };
                r.value().driveIds = std::make_unique<ThrowingIds>();
            }
            return std::move(r).value();
        }, [&](auto r){BOOST_REQUIRE(r); ready = true;}));
        BOOST_REQUIRE(anof::test::waitFor(loop, [&]{return ready;}));
    }
    ~Runtime() {
        drive.requestStop(); metadata.requestStop();
        if (!anof::test::waitFor(loop, [&]{return metadata.snapshot().state == MetadataExecutorState::Stopped && drive.stopped();})) std::terminate();
    }
    template<class T, class C> Result<T> run(C c) {
        std::optional<Result<T>> result;
        auto task = drive.submit(*admin, std::move(c), [&](TaskCompletion<T> completed) {
            BOOST_REQUIRE(std::holds_alternative<Result<T>>(completed.outcome));
            result.emplace(std::move(std::get<Result<T>>(completed.outcome)));
        });
        BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(task));
        BOOST_CHECK(!result);
        BOOST_REQUIRE(anof::test::waitFor(loop, [&]{return result.has_value();}));
        return std::move(*result);
    }
};
struct Gate {
    std::promise<void> release;
    std::shared_future<void> future = release.get_future().share();
    std::atomic<bool> entered{false};
    bool released = false;
    void open() { if (!released) { released = true; release.set_value(); } }
    ~Gate() { open(); }
};
struct ReleaseGate { Gate& gate; ~ReleaseGate() { gate.open(); } };
}
BOOST_AUTO_TEST_CASE(all_commands_share_worker_and_shutdown_drains) {
    Runtime f;
    auto upload = f.publish(f.createUpload(f.personal->rootNodeId, "source"));
    auto file = f.node(*upload.resultNodeId);
    f.start();
    auto moved = take(f.run<NodeDetails>(UpdateNodeCommand{file.metadata.id, NodeCondition{file.metadata.id, file.metadata.revision}, {}, "renamed"}));
    auto copy = take(f.run<NodeDetails>(CopyFileCommand{moved.metadata.id, NodeCondition{moved.metadata.id, moved.metadata.revision}, f.personal->id, f.personal->rootNodeId, "copy"}));
    auto search = take(f.run<Page<NodeDetails, SearchCursor>>(SearchNodesCommand{f.personal->id, SearchText::parse("copy").value(), {}, {}}));
    BOOST_REQUIRE_EQUAL(search.items.size(), 1);
    auto trash = take(f.run<TrashView>(TrashNodeCommand{copy.metadata.id, NodeCondition{copy.metadata.id, copy.metadata.revision}}));
    take(f.run<TrashView>(GetTrashCommand{trash.entry.id}));
    auto list = take(f.run<Page<TrashView, TrashCursor>>(ListTrashCommand{f.personal->id, {}, {}, {}}));
    BOOST_REQUIRE_EQUAL(list.items.size(), 1);
    auto restored = take(f.run<NodeDetails>(RestoreTrashCommand{trash.entry.id, TrashCondition{trash.entry.id, trash.entry.revision}, {}, {}}));
    trash = take(f.run<TrashView>(TrashNodeCommand{restored.metadata.id, NodeCondition{restored.metadata.id, restored.metadata.revision}}));
    BOOST_REQUIRE(f.run<void>(PurgeTrashCommand{trash.entry.id, TrashCondition{trash.entry.id, trash.entry.revision}}));
    BOOST_CHECK_EQUAL(f.drive.outstanding(), 0);
}
BOOST_AUTO_TEST_CASE(capacity_cancel_and_callback_exception_keep_retirement_bounded) {
    Gate gate; Runtime f; ReleaseGate release{gate}; f.start();
    auto blocker = f.metadata.submitOperation([&](auto&) -> Result<void> {gate.entered = true; gate.future.wait(); return {};}, [](auto){});
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(blocker));
    BOOST_REQUIRE(anof::test::waitFor(f.loop, [&]{return gate.entered.load();}));
    SearchNodesCommand command{f.personal->id, SearchText::parse("x").value(), {}, {}};
    unsigned delivered = 0;
    auto first = f.drive.submit(*f.admin, command, [&](TaskCompletion<Page<NodeDetails, SearchCursor>> c) {
        BOOST_CHECK(std::holds_alternative<CancelledBeforeStart>(c.outcome)); ++delivered;
    });
    auto second = f.drive.submit(*f.admin, command, [&](TaskCompletion<Page<NodeDetails, SearchCursor>> c) {
        BOOST_REQUIRE(std::holds_alternative<CancelledBeforeStart>(c.outcome));
        ++delivered; throw std::runtime_error("injected callback failure");
    });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(first)); BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(second));
    auto rejected = f.drive.submit(*f.admin, command, [&](auto){BOOST_FAIL("rejected task callback");});
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(rejected)); BOOST_CHECK(std::get<SubmitError>(rejected) == SubmitError::QueueFull);
    BOOST_CHECK(std::get<AcceptedTask>(first).handle->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    f.drive.requestStop();
    rejected = f.drive.submit(*f.admin, command, [&](auto){BOOST_FAIL("stopping task callback");});
    BOOST_CHECK(std::get<SubmitError>(rejected) == SubmitError::Stopping);
    BOOST_CHECK(!f.drive.stopped());
    f.metadata.requestStop(); gate.open();
    BOOST_REQUIRE(anof::test::waitFor(f.loop, [&]{return f.metadata.snapshot().state == MetadataExecutorState::Stopped;}));
    BOOST_CHECK_EQUAL(delivered, 2); BOOST_CHECK(f.drive.stopped());
    BOOST_CHECK_EQUAL(f.metadata.snapshot().callbackExceptions, 1);
}
BOOST_AUTO_TEST_CASE(write_exception_is_unknown_and_does_not_leak_capacity) {
    Runtime f;
    auto node = f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "folder");
    f.start(true);
    auto result = f.run<TrashView>(TrashNodeCommand{node.metadata.id, NodeCondition{node.metadata.id, node.metadata.revision}});
    BOOST_REQUIRE(!result); BOOST_CHECK(result.error().code == ErrorCode::CommitOutcomeUnknown);
    auto found = take(f.run<Page<NodeDetails, SearchCursor>>(SearchNodesCommand{f.personal->id, SearchText::parse("folder").value(), {}, {}}));
    BOOST_CHECK_EQUAL(found.items.size(), 1); BOOST_CHECK_EQUAL(f.drive.outstanding(), 0);
}

BOOST_AUTO_TEST_CASE(running_write_rejects_cancellation_and_finishes_during_stop) {
    Gate gate; Runtime f; ReleaseGate release{gate};
    auto node = f.mkdir(*f.admin, *f.personal, f.personal->rootNodeId, "before");
    f.resources = {};
    struct GatedClock : Clock {
        std::unique_ptr<Clock> inner;
        Gate& gate;
        mutable bool first = true;
        GatedClock(std::unique_ptr<Clock> c, Gate& g) : inner(std::move(c)), gate(g) {}
        UtcTime now() const override {
            if (first) { first = false; gate.entered = true; gate.future.wait(); }
            return inner->now();
        }
    };
    bool ready = false;
    BOOST_REQUIRE(f.metadata.start([&]() -> Result<MetadataWorkerResources> {
        auto r = makeInstanceMetadataResources(f.root / ".anof/metadata.sqlite3");
        if (!r) return r.error();
        r.value().clock = std::make_unique<GatedClock>(std::move(r.value().clock), gate);
        return std::move(r).value();
    }, [&](auto r){BOOST_REQUIRE(r); ready = true;}));
    BOOST_REQUIRE(anof::test::waitFor(f.loop, [&]{return ready;}));
    unsigned completed = 0;
    auto task = f.drive.submit(*f.admin, UpdateNodeCommand{node.metadata.id, NodeCondition{node.metadata.id, node.metadata.revision}, {}, "after"},
        [&](TaskCompletion<NodeDetails> c) {
            BOOST_REQUIRE(std::holds_alternative<Result<NodeDetails>>(c.outcome));
            auto result = take(std::move(std::get<Result<NodeDetails>>(c.outcome)));
            BOOST_CHECK_EQUAL(result.metadata.name, "after"); ++completed;
        });
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(task));
    BOOST_REQUIRE(anof::test::waitFor(f.loop, [&]{return gate.entered.load();}));
    BOOST_CHECK(std::get<AcceptedTask>(task).handle->cancelBeforeStart() == CancelBeforeStartResult::AlreadyRunning);
    f.drive.requestStop(); f.metadata.requestStop();
    BOOST_CHECK(!f.drive.stopped()); BOOST_CHECK_EQUAL(f.drive.outstanding(), 1);
    gate.open();
    BOOST_REQUIRE(anof::test::waitFor(f.loop, [&]{return f.metadata.snapshot().state == MetadataExecutorState::Stopped;}));
    BOOST_CHECK_EQUAL(completed, 1); BOOST_CHECK(f.drive.stopped());
    f.reopen(); BOOST_CHECK_EQUAL(f.node(node.metadata.id).metadata.name, "after");
}
