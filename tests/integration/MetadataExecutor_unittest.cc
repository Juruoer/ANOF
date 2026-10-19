#include <tests/support/EventLoopWait.h>
#define BOOST_TEST_MODULE AnofMetadataExecutorIntegration
#include <boost/test/unit_test.hpp>

#include <anof/runtime/MetadataExecutor.h>
#include <tests/persistence_contract/TestMetadataBackend.h>

#include <muduox/net/EventLoop.h>

#include <iomanip>
#include <sstream>
#include <thread>

namespace anof::testing {
namespace {
using namespace anof::runtime;

template <typename T>
T id(unsigned int number) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setfill('0') << std::setw(12) << number;
    return T::parse(text.str()).value();
}

struct TestClock final : Clock {
    UtcTime now() const override { return UtcTime{ std::chrono::microseconds{ 1000000 } }; }
};
struct TestIds final : NodeIdGenerator {
    unsigned int sequence = 1000;
    Result<NodeId> next() override { return id<NodeId>(sequence++); }
};
struct Lifetime {
    std::thread::id openedOn;
    std::thread::id closedOn;
};
class ObservedStore final : public MetadataStore {
public:
    ObservedStore(std::unique_ptr<MetadataStore> inner, Lifetime& lifetime)
        : inner_(std::move(inner)), lifetime_(lifetime) { lifetime_.openedOn = std::this_thread::get_id(); }
    ~ObservedStore() override {
        inner_.reset();
        lifetime_.closedOn = std::this_thread::get_id();
    }
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override { return inner_->beginRead(); }
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override { return inner_->beginWrite(); }
private:
    std::unique_ptr<MetadataStore> inner_;
    Lifetime& lifetime_;
};

bool drive(muduox::net::EventLoop& loop, const std::function<bool()>& done,
           const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
    return anof::test::waitFor(loop, done, 3, file, line);
}
struct StopGuard {
    muduox::net::EventLoop& loop;
    MetadataExecutor& executor;
    ~StopGuard() {
        executor.requestStop();
        if (!drive(loop, [&] { return executor.snapshot().state == MetadataExecutorState::Stopped; })) std::terminate();
    }
};

BOOST_AUTO_TEST_CASE(real_sqlite_worker_commits_and_closes_before_loop_stop) {
    const auto owner = id<UserId>(1);
    const auto space = id<SpaceId>(1);
    const auto root = id<NodeId>(1);
    const SessionDigest token{ SessionDigest::Bytes{ 1 } };
    const UtcTime now{ std::chrono::microseconds{ 1000000 } };
    MetadataSeed seed;
    seed.users.push_back({ { owner, UserRole::Member, AuthEpoch{} }, true });
    seed.sessions.push_back({ token, owner, AuthEpoch{}, now + std::chrono::hours(1), false });
    seed.spaces.push_back({ space, SpaceKind::Private, owner, root });
    seed.nodes.push_back({ { root, space, std::nullopt, "", owner, Revision{}, now, now }, std::nullopt, false });
    seed.quotas.push_back({ SpaceQuota{ space }, { { ByteCount{}, NodeCount::from(10).value() },
                                                  { ByteCount{}, NodeCount::from(1).value() }, { ByteCount{}, NodeCount{} } } });
    auto backend = makeTestMetadataBackend();
    BOOST_REQUIRE(backend->reset(seed));
    Lifetime lifetime;
    std::vector<CreateDirectoryCompletion> results;
    const auto loopThread = std::this_thread::get_id();
    muduox::net::EventLoop loop;
    MetadataExecutor executor(loop, { 2, 1 });
    StopGuard cleanup{ loop, executor };
    BOOST_REQUIRE(executor.start([&]() -> Result<MetadataWorkerResources> {
        // 生产工厂同样只能在此 worker 回调内打开；root 测试不直接写 SQL。
        auto store = backend->connect();
        if (!store) return store.error();
        return MetadataWorkerResources{ std::make_unique<ObservedStore>(std::move(store).value(), lifetime),
                                        std::make_unique<TestClock>(), std::make_unique<TestIds>() };
    }));
    BOOST_REQUIRE(drive(loop, [&] { return executor.snapshot().state == MetadataExecutorState::Ready; }));
    const auto submit = [&](SessionDigest digest, const std::string& name) {
        auto accepted = executor.trySubmit({ { digest, CredentialMode::Bearer }, id<RequestId>(1) }, { space, root, name }, [&](auto completion) {
            BOOST_CHECK(std::this_thread::get_id() == loopThread);
            results.push_back(std::move(completion));
        });
        BOOST_REQUIRE(std::holds_alternative<AcceptedTask>(accepted));
    };
    submit(token, "family");
    BOOST_REQUIRE(drive(loop, [&] { return results.size() == 1; }));
    const auto& success = std::get<Result<CreateDirectoryResult>>(results[0].outcome);
    BOOST_REQUIRE(success);
    BOOST_CHECK(success.value().directory.id == id<NodeId>(1000));
    BOOST_CHECK_EQUAL(success.value().parentRevision.value(), 2);
    submit(SessionDigest{ SessionDigest::Bytes{ 2 } }, "rejected");
    BOOST_REQUIRE(drive(loop, [&] { return results.size() == 2; }));
    BOOST_CHECK(std::get<Result<CreateDirectoryResult>>(results[1].outcome).error().code == ErrorCode::Unauthenticated);
    submit(token, "family");
    BOOST_REQUIRE(drive(loop, [&] { return results.size() == 3; }));
    BOOST_CHECK(std::get<Result<CreateDirectoryResult>>(results[2].outcome).error().code == ErrorCode::NameConflict);
    executor.requestStop();
    BOOST_REQUIRE(drive(loop, [&] { return executor.snapshot().state == MetadataExecutorState::Stopped; }));
    BOOST_CHECK(lifetime.openedOn != loopThread);
    BOOST_CHECK(lifetime.closedOn == lifetime.openedOn);
    BOOST_CHECK_EQUAL(executor.snapshot().outstanding, 0U);
    // worker 连接已经关闭；重开后观察持久状态及单一审计，不只检查回调值。
    auto node = backend->readNode(id<NodeId>(1000));
    BOOST_REQUIRE(node);
    BOOST_REQUIRE(node.value());
    BOOST_CHECK_EQUAL(node.value()->metadata.name, "family");
    BOOST_CHECK(!backend->readNode(id<NodeId>(1001)).value());
    BOOST_CHECK(!backend->readNode(id<NodeId>(1002)).value());
    BOOST_CHECK_EQUAL(backend->readNode(root).value()->metadata.revision.value(), 2);
    BOOST_CHECK_EQUAL(backend->readQuota(SpaceQuota{ space }).value().used.nodes.value(), 2);
    auto audit = backend->readAudit();
    BOOST_REQUIRE(audit);
    BOOST_REQUIRE_EQUAL(audit.value().size(), 1U);
    BOOST_CHECK(audit.value()[0].requestId == id<RequestId>(1));
}

}  // namespace
}  // namespace anof::testing
