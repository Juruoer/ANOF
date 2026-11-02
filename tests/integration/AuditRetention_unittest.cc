#define BOOST_TEST_MODULE AuditRetentionRuntime
#include <anof/runtime/AuditRetentionExecutor.h>
#include <anof/runtime/InstanceIdentity.h>
#include <muduox/net/EventLoop.h>
#include <tests/integration/ClientProcess.h>
#include <tests/integration/InitializedInstance.h>
#include <tests/support/EventLoopWait.h>

#include <atomic>
#include <boost/test/unit_test.hpp>
#include <thread>
using namespace anof;
using namespace anof::runtime;
namespace {
struct State {
    std::atomic<int> expired{ 0 }, runs{ 0 };
    std::atomic<std::int64_t> cutoff{ 0 };
    std::atomic<bool> wrongThread{ false };
    bool unknown = false;
    std::thread::id main = std::this_thread::get_id();
};
class FixedClock : public Clock {
public:
    UtcTime now() const override { return UtcTime(std::chrono::hours(24 * 100)); }
};
class Retention : public AuditRetention {
    std::shared_ptr<State> s;

public:
    explicit Retention(std::shared_ptr<State> state) : s(std::move(state)) {}
    Result<AuditRetentionResult> eraseExpired(const AuditRetentionRequest& r) override {
        ++s->expired;
        s->cutoff = r.before.time_since_epoch().count();
        s->wrongThread = std::this_thread::get_id() == s->main;
        if (s->unknown) return ErrorCode::CommitOutcomeUnknown;
        return AuditRetentionResult{ {}, false };
    }
    Result<AuditRetentionResult> eraseCompletedRuns(UtcTime, AuditBatchSize) override {
        ++s->runs;
        return AuditRetentionResult{ {}, false };
    }
};
void exercise(bool unknown) {
    anof::testing::cli::Temp tmp;
    auto root = tmp.path / "source";
    BOOST_REQUIRE_EQUAL(::mkdir(root.c_str(), 0700), 0);
    BOOST_REQUIRE(anof::testing::initializeTestInstance(root));
    auto state = std::make_shared<State>();
    state->unknown = unknown;
    muduox::net::EventLoop loop;
    MetadataExecutor metadata(loop);
    AuditRetentionExecutor audit(loop, metadata, AuditRetentionDays::from(90).value(), AuditBatchSize::from(1).value());
    bool ready = false, stopped = false, retired = false;
    BOOST_REQUIRE(metadata.start([&]() -> Result<MetadataWorkerResources> {auto r=makeInstanceMetadataResources(root/".anof/metadata.sqlite3");if(!r)return r.error();r.value().auditRetention=std::make_unique<Retention>(state);r.value().clock=std::make_unique<FixedClock>();return std::move(r).value(); }, [&](auto r) {BOOST_REQUIRE(r);ready=true; }, [&] { stopped = true; }));
    BOOST_REQUIRE(anof::test::waitFor(loop, [&] { return ready; }, 3));
    audit.start();
    audit.tick();
    if (unknown) {
        BOOST_REQUIRE(anof::test::waitFor(loop, [&] { return audit.paused(); }, 3));
        audit.tick();
    }
    audit.requestStop([&] {retired=true;metadata.requestStop(); });
    BOOST_REQUIRE(anof::test::waitFor(loop, [&] { return stopped; }, 3));
    BOOST_CHECK(retired);
    BOOST_CHECK(audit.stopped());
    BOOST_CHECK_EQUAL(state->expired.load(), 1);
    BOOST_CHECK_EQUAL(state->runs.load(), unknown ? 0 : 1);
    BOOST_CHECK_EQUAL(state->cutoff.load(), 10LL * 86400 * 1000000);
    BOOST_CHECK(!state->wrongThread.load());
}
}  // namespace
BOOST_AUTO_TEST_CASE(audit_runs_on_metadata_worker_and_retires_before_worker_stop) { exercise(false); }
BOOST_AUTO_TEST_CASE(unknown_commit_pauses_retention_without_automatic_retry) { exercise(true); }
