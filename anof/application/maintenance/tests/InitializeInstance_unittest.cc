#define BOOST_TEST_MODULE InitializeInstance
#include <boost/test/unit_test.hpp>
#include <anof/application/maintenance/InitializeInstance.h>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace anof;
namespace {
template<class T> T id(char digit = '1') {
    std::string text = "00000000-0000-4000-8000-000000000001";
    text.back() = digit;
    return T::parse(text).value();
}
struct State {
    std::vector<InitStage> calls;
    std::optional<InitStage> fail;
    std::optional<ByteCount> budget;
    std::optional<DiskSafetyPolicy> safety;
    bool throws = false, held = false, badReceipt = false;
    std::optional<InitFailure> visit(InitStage stage) {
        calls.push_back(stage);
        if (fail == stage) {
            if (throws) throw std::runtime_error("injected failure");
            return InitFailure{stage == InitStage::Bootstrap ? InitError::CommitOutcomeUnknown : InitError::IoError, stage};
        }
        return {};
    }
};
struct Lease final : InitializationLease, InstanceLease {
    State& state;
    std::filesystem::path path = "/test/instance";
    InitializationIdentity identityValue{id<InstanceId>(), id<OperationId>()};
    explicit Lease(State& s) : state(s) { state.held = true; }
    ~Lease() override { state.held = false; }
    const InstanceLease& lease() const override { return *this; }
    const std::filesystem::path& root() const noexcept override { return path; }
    const InitializationIdentity& identity() const override { return identityValue; }
    InitResult<void> writeConfiguration(const InstanceConfiguration& config) override {
        state.safety = config.diskSafety;
        if (auto failure = state.visit(InitStage::WriteConfiguration)) return *failure;
        return {};
    }
    InitResult<void> publishFormat() override {
        if (auto failure = state.visit(InitStage::PublishFormat)) return *failure;
        return {};
    }
    InitResult<void> clearPending() override {
        if (auto failure = state.visit(InitStage::ClearPending)) return *failure;
        return {};
    }
};
struct Files final : InstanceInitializer {
    State& state;
    explicit Files(State& s) : state(s) {}
    InitResult<std::unique_ptr<InitializationLease>> acquire(InitStage stage) {
        auto result = state.visit(stage);
        if (result) return *result;
        return std::unique_ptr<InitializationLease>(new Lease(state));
    }
    InitResult<std::unique_ptr<InitializationLease>> create(const std::filesystem::path&, const InitializationIdentity&) override {
        return acquire(InitStage::CreateDirectory);
    }
    InitResult<std::unique_ptr<InitializationLease>> openPending(const std::filesystem::path&) override { return acquire(InitStage::AcquireLock); }
};
struct Database final : InstanceBootstrap {
    State& state;
    explicit Database(State& s) : state(s) {}
    InitResult<InitReceipt> result(InitStage stage) {
        BOOST_CHECK(state.held);
        auto result = state.visit(stage);
        if (result) return *result;
        return InitReceipt{{id<InstanceId>(state.badReceipt ? '9' : '1'), id<OperationId>()}, id<UserId>(), id<SpaceId>('2'), id<SpaceId>('3')};
    }
    InitResult<InitReceipt> createCurrent(const InstanceLease&, const TransferBootstrapSeed& seed) override {
        state.budget = seed.contentBudget;
        return result(InitStage::Bootstrap);
    }
    InitResult<InitReceipt> verifyComplete(const InstanceLease&, const InitializationIdentity&) override { return result(InitStage::Verify); }
};
struct Passwords final : PasswordHasher {
    State& state;
    explicit Passwords(State& s) : state(s) {}
    Result<EncodedPasswordHash> hash(const PasswordInput&) override {
        BOOST_CHECK(!state.held);
        auto result = state.visit(InitStage::HashPassword);
        if (result) return ErrorCode::InternalError;
        return EncodedPasswordHash::parse("$argon2id$v=19$m=8192,t=1,p=1$" + std::string(22, 'A') + "$" + std::string(43, 'A'), {8192, 1, 1});
    }
    Result<bool> verify(const PasswordInput&, const EncodedPasswordHash&) override { BOOST_FAIL("Initialization must not verify passwords"); return false; }
};
struct Ids final : IdentityIdGenerator {
    char space = '2', node = '4';
    Result<UserId> nextUser() override { return id<UserId>(); }
    Result<SpaceId> nextSpace() override { return id<SpaceId>(space++); }
    Result<NodeId> nextNode() override { return id<NodeId>(node++); }
    Result<InviteId> nextInvite() override { BOOST_FAIL("No invitation during bootstrap"); return ErrorCode::InternalError; }
    Result<InstanceId> nextInstance() override { return id<InstanceId>(); }
    Result<OperationId> nextOperation() override { return id<OperationId>(); }
};
struct Time final : Clock { UtcTime now() const override { return {}; } };
InitOptions options() {
    const QuotaDelta quota{ByteCount{}, NodeCount::from(1).value()};
    InstanceConfiguration config; config.developmentHttp = true;
    config.diskSafety = {ByteCount::from(12345).value(), SafetyPercent::from(17).value()};
    return {"/test/instance", LoginName::parse("admin").value(), DisplayName::parse("Admin").value(),
        {quota, quota, quota}, UserLimit{}, config};
}
struct Fixture {
    State state;
    Files files{state}; Database db{state}; Passwords passwords{state}; Ids ids; Time time;
    InitResult<InitReceipt> run() {
        return InitializeInstance(files, db, passwords, ids, time).execute(TransferInitOptions{options(), ByteCount::from(4096).value()}, PasswordInput::parse("test password here").value(), id<RequestId>());
    }
};
}
BOOST_AUTO_TEST_CASE(sequence_and_lease_cover_all_writes) {
    Fixture f;
    BOOST_REQUIRE(f.run());
    BOOST_REQUIRE(f.state.budget);
    BOOST_CHECK_EQUAL(f.state.budget->value(), 4096);
    BOOST_REQUIRE(f.state.safety);
    BOOST_CHECK_EQUAL(f.state.safety->minimum.value(), 12345);
    BOOST_CHECK_EQUAL(f.state.safety->percent.value(), 17);
    static_assert(InitializationIdentity::schemaVersion == 3 && InitializationIdentity::formatVersion == 3);
    BOOST_CHECK(!f.state.held);
    const std::vector<InitStage> expected{InitStage::HashPassword, InitStage::CreateDirectory, InitStage::WriteConfiguration,
        InitStage::Bootstrap, InitStage::PublishFormat, InitStage::ClearPending};
    BOOST_CHECK(f.state.calls == expected);
}
BOOST_AUTO_TEST_CASE(each_failure_stops_without_retry_or_publication) {
    for (auto stage : {InitStage::HashPassword, InitStage::CreateDirectory, InitStage::WriteConfiguration,
            InitStage::Bootstrap, InitStage::PublishFormat, InitStage::ClearPending}) {
        Fixture f; f.state.fail = stage;
        auto result = f.run();
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().stage == stage);
        BOOST_CHECK(f.state.calls.back() == stage);
        BOOST_CHECK(!f.state.held);
        if (stage == InitStage::Bootstrap) BOOST_CHECK(result.error().code == InitError::CommitOutcomeUnknown);
    }
}
BOOST_AUTO_TEST_CASE(exceptions_preserve_uncertain_write_classification) {
    for (auto stage : {InitStage::Bootstrap, InitStage::PublishFormat, InitStage::ClearPending}) {
        Fixture f; f.state.fail = stage; f.state.throws = true;
        auto result = f.run();
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == (stage == InitStage::Bootstrap ? InitError::CommitOutcomeUnknown : InitError::PublicationOutcomeUnknown));
        BOOST_CHECK(!f.state.held);
    }
}
BOOST_AUTO_TEST_CASE(finish_verifies_before_publication_and_never_recreates_or_hashes) {
    Fixture f;
    BOOST_REQUIRE(FinishInitialization(f.files, f.db).execute("/test/instance"));
    const std::vector<InitStage> expected{InitStage::AcquireLock, InitStage::Verify, InitStage::PublishFormat, InitStage::ClearPending};
    BOOST_CHECK(f.state.calls == expected);
    BOOST_CHECK(!f.state.held);
    Fixture failed; failed.state.fail = InitStage::Verify;
    BOOST_CHECK(!FinishInitialization(failed.files, failed.db).execute("/test/instance"));
    BOOST_CHECK(failed.state.calls.back() == InitStage::Verify);
}
BOOST_AUTO_TEST_CASE(invalid_input_and_receipt_prevent_side_effects) {
    Fixture f;
    auto input = options(); input.configuration.developmentHttp = false;
    auto result = InitializeInstance(f.files, f.db, f.passwords, f.ids, f.time).execute(TransferInitOptions{std::move(input), ByteCount::from(4096).value()}, PasswordInput::parse("test password here").value(), id<RequestId>());
    BOOST_CHECK(!result);
    BOOST_CHECK(f.state.calls.empty());
    f.state.badReceipt = true;
    BOOST_CHECK(!f.run());
    BOOST_CHECK(f.state.calls.back() == InitStage::Bootstrap);
}
