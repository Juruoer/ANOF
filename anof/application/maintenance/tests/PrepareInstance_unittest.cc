#define BOOST_TEST_MODULE PrepareInstance
#include <boost/test/unit_test.hpp>

#include <anof/application/maintenance/PrepareInstance.h>
#include <limits>
#include <stdexcept>
#include <vector>

namespace anof::testing {
namespace {
struct State {
    bool held = false;
    std::vector<int> calls;
};
class Lease final : public InstanceLease {
public:
    explicit Lease(State& state) : state_(state) { state_.held = true; }
    ~Lease() override { state_.held = false; }
    const std::filesystem::path& root() const noexcept override { return root_; }
private:
    State& state_;
    std::filesystem::path root_{ "/test-owned-instance" };
};
struct Dependencies final : InstanceLockProvider, InstanceConfigurationLoader, InstanceMaintenance {
    State state;
    InstanceConfiguration configuration;
    int failure = 0;
    bool throwFailure = false;
    Dependencies() { configuration.developmentHttp = true; }
    Result<std::unique_ptr<InstanceLease>> acquire(const std::filesystem::path&) override {
        state.calls.push_back(1);
        if (state.held || failure == 1) return ErrorCode::Conflict;
        return std::unique_ptr<InstanceLease>(new Lease(state));
    }
    Result<InstanceConfiguration> load(const InstanceLease&) override {
        BOOST_CHECK(state.held);
        state.calls.push_back(2);
        if (failure == 2) {
            if (throwFailure) throw std::runtime_error("injected load failure");
            return ErrorCode::InvalidArgument;
        }
        return configuration;
    }
    Result<void> verify(const InstanceLease&) override {
        BOOST_CHECK(state.held);
        state.calls.push_back(3);
        if (failure == 3) {
            if (throwFailure) throw std::runtime_error("injected maintenance failure");
            return ErrorCode::IntegrityError;
        }
        return {};
    }
    Result<PreparedInstance> prepare() { return prepareInstance("/test-owned-instance", *this, *this, *this); }
};

BOOST_AUTO_TEST_CASE(lock_covers_configuration_maintenance_and_owned_result) {
    Dependencies dependencies;
    {
        auto result = dependencies.prepare();
        BOOST_REQUIRE(result);
        BOOST_CHECK(dependencies.state.held);
        const std::vector<int> expected{ 1, 2, 3 };
        BOOST_CHECK(dependencies.state.calls == expected);
        auto second = dependencies.prepare();
        BOOST_REQUIRE(!second);
        BOOST_CHECK(second.error().code == ErrorCode::Conflict);
        BOOST_CHECK(dependencies.state.held);
        BOOST_CHECK_EQUAL(dependencies.state.calls.size(), 4U);
    }
    BOOST_CHECK(!dependencies.state.held);
}

BOOST_AUTO_TEST_CASE(failed_stage_does_not_run_later_stages_or_retain_lock) {
    for (int stage : { 1, 2, 3 }) {
        Dependencies dependencies;
        dependencies.failure = stage;
        auto result = dependencies.prepare();
        BOOST_REQUIRE(!result);
        BOOST_CHECK_EQUAL(dependencies.state.calls.size(), static_cast<std::size_t>(stage));
        BOOST_CHECK(!dependencies.state.held);
    }
}

BOOST_AUTO_TEST_CASE(throwing_adapter_unwinds_lock) {
    for (int stage : { 2, 3 }) {
        Dependencies dependencies;
        dependencies.failure = stage;
        dependencies.throwFailure = true;
        auto result = dependencies.prepare();
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::InternalError);
        BOOST_CHECK(!dependencies.state.held);
    }
}

BOOST_AUTO_TEST_CASE(invalid_configuration_never_opens_maintenance) {
    Dependencies dependencies;
    dependencies.configuration.developmentHttp = false;
    auto result = dependencies.prepare();
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == ErrorCode::InvalidArgument);
    BOOST_CHECK_EQUAL(dependencies.state.calls.size(), 2U);
    BOOST_CHECK(!dependencies.state.held);
}

BOOST_AUTO_TEST_CASE(empty_lock_success_is_rejected_before_reading_configuration) {
    struct BrokenLock final : InstanceLockProvider {
        Result<std::unique_ptr<InstanceLease>> acquire(const std::filesystem::path&) override {
            return std::unique_ptr<InstanceLease>{};
        }
    } locks;
    Dependencies dependencies;
    auto result = prepareInstance("/test-owned-instance", locks, dependencies, dependencies);
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == ErrorCode::IntegrityError);
    BOOST_CHECK(dependencies.state.calls.empty());
}

BOOST_AUTO_TEST_CASE(configuration_rejects_unbounded_or_remote_settings) {
    InstanceConfiguration valid;
    valid.developmentHttp = true;
    valid.port = 0;
    BOOST_CHECK(validateInstanceConfiguration(valid));
    auto invalid = valid;
    invalid.listenAddress = "0.0.0.0";
    BOOST_CHECK(!validateInstanceConfiguration(invalid));
    for (auto count : { 0U, 257U }) {
        invalid = valid; invalid.metadataQueue = count;
        BOOST_CHECK(!validateInstanceConfiguration(invalid));
    }
    for (auto count : { 0U, 129U }) {
        invalid = valid; invalid.httpInFlight = count;
        BOOST_CHECK(!validateInstanceConfiguration(invalid));
    }
    for (auto seconds : { -1.0, 30.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        invalid = valid; invalid.shutdownGraceSeconds = seconds;
        BOOST_CHECK(!validateInstanceConfiguration(invalid));
    }
    valid.shutdownGraceSeconds = 0;
    BOOST_CHECK(validateInstanceConfiguration(valid));
}
}  // namespace
}  // namespace anof::testing
