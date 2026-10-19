#define BOOST_TEST_MODULE DiskExecutor
#include <anof/runtime/DiskExecutor.h>
#include <muduox/net/EventLoop.h>
#include <tests/support/EventLoopWait.h>

#include <atomic>
#include <boost/test/unit_test.hpp>
#include <condition_variable>
#include <mutex>
using namespace anof;
using namespace anof::runtime;
namespace {
struct Store : TransferFileStore {
    FileResult<OwnedTransferFile> create(UploadId) override { return TransferFileError::Transient; }
    FileResult<OwnedTransferFile> open(const TransferFileKey&, bool) override { return TransferFileError::Transient; }
    FileResult<TransferFileStat> stat(TransferFileHandle&) override { return TransferFileError::Transient; }
    FileStatus write(TransferFileHandle&, UploadOffset, const TransferBlock&) override {
        return TransferFileError::Transient;
    }
    FileStatus read(TransferFileHandle&, UploadOffset, std::size_t, TransferBlock&) override {
        return TransferFileError::Transient;
    }
    FileStatus syncData(TransferFileHandle&) override { return TransferFileError::Transient; }
    FileStatus truncateAndSync(TransferFileHandle&, UploadOffset) override { return TransferFileError::Transient; }
    FileStatus publish(UploadId, BlobId) override { return TransferFileError::Transient; }
    FileStatus syncPublication(UploadId, BlobId) override { return TransferFileError::Transient; }
    FileStatus removeAndSync(const TransferFileKey&) override { return TransferFileError::Transient; }
    FileResult<OwnedTransferHash> beginHash() override { return TransferFileError::Transient; }
    FileStatus updateHash(TransferHashState&, const TransferBlock&) override { return TransferFileError::Transient; }
    FileResult<Sha256> finishHash(OwnedTransferHash) override { return TransferFileError::Transient; }
    FileResult<Page<InventoryEntry, InventoryCursor>> inventory(InventoryArea, PageSize,
                                                                std::optional<InventoryCursor>) override {
        return TransferFileError::Transient;
    }
    FileResult<DiskCapacity> capacity() override { return TransferFileError::Transient; }
};
Result<std::unique_ptr<TransferFileStore>> factory() { return std::unique_ptr<TransferFileStore>(new Store); }
struct Fixture {
    muduox::net::EventLoop loop;
    DiskExecutor disk{ loop, { 2, 1 } };
    std::mutex mutex;
    std::condition_variable changed;
    bool release = false;
    std::atomic<unsigned> entered{ 0 };
    Fixture() {
        bool ready = false;
        BOOST_REQUIRE(disk.start(factory,
                                 [&](auto result) {
                                     BOOST_REQUIRE(result);
                                     ready = true;
                                 },
                                 {}));
        BOOST_REQUIRE(test::waitFor(loop, [&] { return ready; }));
    }
    void unlock() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            release = true;
        }
        changed.notify_all();
    }
    ~Fixture() {
        unlock();
        disk.requestStop();
        if (!test::waitFor(loop, [this] { return disk.snapshot().stopped; })) std::terminate();
    }
    Result<void> gate(TransferFileStore&) {
        std::unique_lock<std::mutex> lock(mutex);
        ++entered;
        changed.wait(lock, [this] { return release; });
        return {};
    }
};
}  // namespace
BOOST_AUTO_TEST_CASE(two_lanes_reserve_cleanup_and_retire_cancelled_completions) {
    Fixture f;
    unsigned callbacks = 0, cancelled = 0;
    auto cb = [&](TaskCompletion<void> done) {
        ++callbacks;
        if (std::holds_alternative<CancelledBeforeStart>(done.outcome)) ++cancelled;
    };
    for (std::size_t lane = 0; lane < 2; ++lane)
        BOOST_REQUIRE(
            std::holds_alternative<AcceptedTask>(f.disk.submit([&](auto& s) { return f.gate(s); }, cb, false, lane)));
    BOOST_REQUIRE(test::waitFor(f.loop, [&] { return f.entered == 2; }));
    auto normal = f.disk.submit([](auto&) -> Result<void> { return {}; }, cb);
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(normal));
    BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(f.disk.submit([](auto&) -> Result<void> { return {}; }, cb)));
    BOOST_CHECK(std::holds_alternative<SubmitError>(f.disk.submit([](auto&) -> Result<void> { return {}; }, cb)));
    BOOST_REQUIRE(
        std::holds_alternative<AcceptedTask>(f.disk.submit([](auto&) -> Result<void> { return {}; }, cb, true)));
    BOOST_CHECK_EQUAL(f.disk.snapshot().outstanding, 5);
    BOOST_CHECK(std::get<AcceptedTask>(normal).handle->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    f.disk.requestStop();
    BOOST_CHECK(!f.disk.snapshot().stopped);
    BOOST_CHECK_EQUAL(callbacks, 0);
    f.unlock();
    BOOST_REQUIRE(test::waitFor(f.loop, [&] { return f.disk.snapshot().stopped; }));
    BOOST_CHECK_EQUAL(callbacks, 5);
    BOOST_CHECK_EQUAL(cancelled, 1);
    BOOST_CHECK_EQUAL(f.disk.snapshot().outstanding, 0);
}
BOOST_AUTO_TEST_CASE(startup_failure_waits_for_both_thread_exits) {
    muduox::net::EventLoop loop;
    DiskExecutor disk(loop);
    bool reported = false, stopped = false;
    BOOST_REQUIRE(disk.start([]() -> Result<std::unique_ptr<TransferFileStore>> { return ErrorCode::IntegrityError; },
                             [&](auto r) {
                                 BOOST_CHECK(!r);
                                 reported = true;
                             },
                             [&] { stopped = true; }));
    BOOST_REQUIRE(test::waitFor(loop, [&] { return stopped; }));
    BOOST_CHECK(reported);
    BOOST_CHECK(disk.snapshot().stopped);
}
