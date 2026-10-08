#define BOOST_TEST_MODULE AnofCreateDirectoryIntegration
#include <boost/test/unit_test.hpp>

#include <anof/application/drive/CreateDirectory.h>
#include <tests/persistence_contract/TestMetadataBackend.h>

#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace anof::testing {
namespace {

template <typename T>
T id(unsigned int number) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setfill('0') << std::setw(12) << number;
    return T::parse(text.str()).value();
}
QuotaDelta amount(std::int64_t bytes, std::int64_t nodes) {
    return { ByteCount::from(bytes).value(), NodeCount::from(nodes).value() };
}

struct TestClock final : Clock {
    UtcTime first{ std::chrono::microseconds{ 1000000 } };
    UtcTime second = first + std::chrono::microseconds{ 10 };
    mutable int calls = 0;
    UtcTime now() const override { return calls++ == 0 ? first : second; }
};
struct TestIds final : NodeIdGenerator {
    NodeId value = id<NodeId>(1000);
    int calls = 0;
    bool fail = false;
    Result<NodeId> next() override {
        ++calls;
        if (fail) return ErrorCode::InternalError;
        return value;
    }
};

// 装饰真实仓储，仅在指定边界截断控制流；不模拟 SQL、提交或回滚状态。
enum class Fault { None, Begin, FirstCharge, SecondCharge, Insert, Revision, Audit, ThrowAfterAudit,
                   BeforeCommit, UnknownRolledBack, UnknownCommitted, ChangedIdentity };
struct Control {
    Fault fault = Fault::None;
    int begins = 0;
    int authorizations = 0;
    int charges = 0;
    int commits = 0;
    std::function<void()> onFirstCharge;
};
class ObservedTransaction final : public MetadataTransaction, public AuthorizationRepository,
                                  public SpaceRepository, public NodeRepository, public QuotaRepository,
                                  public AuditRepository {
public:
    ObservedTransaction(std::unique_ptr<MetadataTransaction> inner, Control& control)
        : inner_(std::move(inner)), control_(control) {}
    AuthorizationRepository& authorization() override { return *this; }
    SpaceRepository& spaces() override { return *this; }
    NodeRepository& nodes() override { return *this; }
    QuotaRepository& quotas() override { return *this; }
    AuditRepository& audit() override { return *this; }
    Result<Principal> requireActiveSession(const SessionDigest& digest, UtcTime now) override {
        ++control_.authorizations;
        auto result = inner_->authorization().requireActiveSession(digest, now);
        if (result && control_.fault == Fault::ChangedIdentity && control_.authorizations == 2) {
            result.value().authEpoch = AuthEpoch::from(result.value().authEpoch.value() + 1).value();
        }
        return result;
    }
    Result<std::optional<Space>> find(SpaceId space) override { return inner_->spaces().find(space); }
    Result<DirectoryPath> findActiveDirectoryPath(SpaceId space, NodeId parent) override {
        return inner_->nodes().findActiveDirectoryPath(space, parent);
    }
    Result<DirectorySnapshot> insertDirectory(const NewDirectory& directory) override {
        auto result = inner_->nodes().insertDirectory(directory);
        if (result && control_.fault == Fault::Insert) return ErrorCode::InternalError;
        return result;
    }
    Result<Revision> advanceRevision(NodeId node, Revision expected, UtcTime now) override {
        auto result = inner_->nodes().advanceRevision(node, expected, now);
        if (result && control_.fault == Fault::Revision) return ErrorCode::InternalError;
        return result;
    }
    Result<void> chargeUsed(const QuotaScope& scope, QuotaDelta delta) override {
        ++control_.charges;
        if (control_.charges == 1 && control_.onFirstCharge) control_.onFirstCharge();
        auto result = inner_->quotas().chargeUsed(scope, delta);
        if (result && ((control_.fault == Fault::FirstCharge && control_.charges == 1) ||
                       (control_.fault == Fault::SecondCharge && control_.charges == 2))) {
            return ErrorCode::InternalError;
        }
        return result;
    }
    Result<void> append(const DirectoryCreatedEvent& event) override {
        auto result = inner_->audit().append(event);
        if (result && control_.fault == Fault::ThrowAfterAudit) throw std::runtime_error("Injected audit boundary failure");
        if (result && control_.fault == Fault::Audit) return ErrorCode::InternalError;
        return result;
    }
    Result<void> commit() override {
        ++control_.commits;
        if (control_.fault == Fault::BeforeCommit || control_.fault == Fault::UnknownRolledBack) {
            inner_.reset();
            return control_.fault == Fault::BeforeCommit ? ErrorCode::Retryable : ErrorCode::CommitOutcomeUnknown;
        }
        auto result = inner_->commit();
        if (result && control_.fault == Fault::UnknownCommitted) return ErrorCode::CommitOutcomeUnknown;
        return result;
    }
private:
    std::unique_ptr<MetadataTransaction> inner_;
    Control& control_;
};
class ObservedStore final : public MetadataStore {
public:
    ObservedStore(MetadataStore& inner, Control& control) : inner_(inner), control_(control) {}
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override {
        ++control_.begins;
        if (control_.fault == Fault::Begin) return ErrorCode::Retryable;
        auto result = inner_.beginWrite();
        if (!result) return result.error();
        return std::unique_ptr<MetadataTransaction>(new ObservedTransaction(std::move(result).value(), control_));
    }
private:
    MetadataStore& inner_;
    Control& control_;
};
struct Fixture {
    UserId owner = id<UserId>(1);
    UserId admin = id<UserId>(2);
    SpaceId privateSpace = id<SpaceId>(1);
    SpaceId publicSpace = id<SpaceId>(2);
    NodeId privateRoot = id<NodeId>(1);
    NodeId publicRoot = id<NodeId>(2);
    SessionDigest ownerSession{ SessionDigest::Bytes{ 1 } };
    SessionDigest adminSession{ SessionDigest::Bytes{ 2 } };
    TestClock clock;
    TestIds ids;
    MetadataSeed seed{
        { { { owner, UserRole::Member, AuthEpoch{} }, true }, { { admin, UserRole::Administrator, AuthEpoch{} }, true } },
        { { ownerSession, owner, AuthEpoch{}, clock.first + std::chrono::hours(1), false },
          { adminSession, admin, AuthEpoch{}, clock.first + std::chrono::hours(1), false } },
        { { privateSpace, SpaceKind::Private, owner, privateRoot }, { publicSpace, SpaceKind::Public, std::nullopt, publicRoot } },
        {},
        { { { privateRoot, privateSpace, std::nullopt, "", owner, Revision{}, clock.first, clock.first }, std::nullopt, false },
          { { publicRoot, publicSpace, std::nullopt, "", admin, Revision{}, clock.first, clock.first }, std::nullopt, false } },
        { { SpaceQuota{ privateSpace }, { amount(1024, 200), amount(0, 1), amount(0, 0) } },
          { SpaceQuota{ publicSpace }, { amount(1024, 200), amount(0, 1), amount(0, 0) } },
          { PublicContributionQuota{ publicSpace, owner }, { amount(1024, 200), amount(0, 0), amount(0, 0) } },
          { PublicContributionQuota{ publicSpace, admin }, { amount(1024, 200), amount(0, 0), amount(0, 0) } } }
    };
    std::unique_ptr<TestMetadataBackend> backend;
    std::unique_ptr<MetadataStore> store;
    Control control;
    std::optional<CreateDirectoryAttempt> attempt;

    void open() {
        backend = makeTestMetadataBackend();
        BOOST_REQUIRE(backend);
        BOOST_REQUIRE(backend->reset(seed));
        auto connected = backend->connect();
        BOOST_REQUIRE(connected);
        store = std::move(connected).value();
    }
    RequestContext context(bool asAdmin = false) const {
        return { asAdmin ? adminSession : ownerSession, id<RequestId>(1) };
    }
    CreateDirectoryCommand command(bool isPublic = true) const {
        return { isPublic ? publicSpace : privateSpace, isPublic ? publicRoot : privateRoot, "new directory" };
    }
    Result<CreateDirectoryResult> run(const CreateDirectoryCommand& command, bool asAdmin = false) {
        ObservedStore observed(*store, control);
        CreateDirectory service(observed, clock, ids);
        return service.execute(context(asAdmin), command, &attempt);
    }
    void unchanged() {
        auto node = backend->readNode(ids.value);
        BOOST_REQUIRE(node);
        BOOST_CHECK(!node.value());
        for (const auto& original : seed.nodes) {
            auto current = backend->readNode(original.metadata.id);
            BOOST_REQUIRE(current);
            BOOST_REQUIRE(current.value());
            BOOST_CHECK(current.value()->metadata.revision == original.metadata.revision);
            BOOST_CHECK(current.value()->metadata.updatedAt == original.metadata.updatedAt);
        }
        for (const auto& original : seed.quotas) {
            auto current = backend->readQuota(original.scope);
            BOOST_REQUIRE(current);
            BOOST_CHECK(current.value().used.bytes == original.state.used.bytes);
            BOOST_CHECK(current.value().used.nodes == original.state.used.nodes);
            BOOST_CHECK(current.value().reserved.bytes == original.state.reserved.bytes);
            BOOST_CHECK(current.value().reserved.nodes == original.state.reserved.nodes);
        }
        auto audit = backend->readAudit();
        BOOST_REQUIRE(audit);
        BOOST_CHECK(audit.value().empty());
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(private_and_public_creation_persist_exactly_one_event) {
    for (int scenario = 0; scenario < 3; ++scenario) {
        Fixture f;
        const bool isPublic = scenario != 0;
        const bool asAdmin = scenario == 2;
        f.open();
        const auto command = f.command(isPublic);
        auto result = f.run(command, asAdmin);
        BOOST_REQUIRE(result);
        const auto& directory = result.value().directory;
        BOOST_CHECK(directory.id == f.ids.value);
        BOOST_CHECK(directory.createdBy == (asAdmin ? f.admin : f.owner));
        BOOST_CHECK(directory.spaceId == command.spaceId);
        BOOST_CHECK(directory.parentId == command.parentId);
        BOOST_TEST(directory.name == command.name);
        BOOST_TEST(directory.revision.value() == 1);
        BOOST_CHECK(directory.createdAt == f.clock.first);
        BOOST_CHECK(directory.updatedAt == f.clock.first);
        BOOST_TEST(result.value().parentRevision.value() == 2);
        BOOST_TEST(f.control.authorizations == 2);
        BOOST_TEST(f.control.commits == 1);
        BOOST_TEST(f.control.charges == (isPublic ? 2 : 1));
        BOOST_TEST(f.ids.calls == 1);
        f.store.reset();
        auto node = f.backend->readNode(directory.id);
        BOOST_REQUIRE(node);
        BOOST_REQUIRE(node.value());
        BOOST_TEST(node.value()->metadata.name == command.name);
        auto parent = f.backend->readNode(command.parentId);
        BOOST_REQUIRE(parent);
        BOOST_REQUIRE(parent.value());
        BOOST_TEST(parent.value()->metadata.revision.value() == 2);
        auto usage = f.backend->readQuota(SpaceQuota{ command.spaceId });
        BOOST_REQUIRE(usage);
        BOOST_TEST(usage.value().used.nodes.value() == 2);
        BOOST_TEST(usage.value().used.bytes.value() == 0);
        if (isPublic) {
            auto personal = f.backend->readQuota(PublicContributionQuota{ f.publicSpace, directory.createdBy });
            BOOST_REQUIRE(personal);
            BOOST_TEST(personal.value().used.nodes.value() == 1);
        }
        auto audit = f.backend->readAudit();
        BOOST_REQUIRE(audit);
        BOOST_REQUIRE_EQUAL(audit.value().size(), 1U);
        BOOST_CHECK(audit.value()[0].actor == directory.createdBy);
        BOOST_CHECK(audit.value()[0].nodeId == directory.id);
        BOOST_CHECK(audit.value()[0].spaceId == command.spaceId);
        BOOST_CHECK(audit.value()[0].parentId == command.parentId);
        BOOST_CHECK(audit.value()[0].requestId == f.context().requestId);
        BOOST_CHECK(audit.value()[0].occurredAt == f.clock.second);
    }
}

BOOST_AUTO_TEST_CASE(admin_and_other_member_cannot_create_in_someone_elses_private_space) {
    for (bool asAdmin : { false, true }) {
        Fixture f;
        if (!asAdmin) f.seed.spaces[0].ownerId = f.admin;
        f.open();
        auto result = f.run(f.command(false), asAdmin);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::NotFound);
        BOOST_TEST(f.control.charges == 0);
        f.unchanged();
    }
}

BOOST_AUTO_TEST_CASE(invalid_authentication_never_writes) {
    for (int scenario = 0; scenario < 5; ++scenario) {
        Fixture f;
        if (scenario == 0) f.seed.sessions[0].revoked = true;
        if (scenario == 1) f.seed.sessions[0].expiresAt = f.clock.first;
        if (scenario == 2) f.seed.users[0].enabled = false;
        if (scenario == 3) f.seed.users[0].principal.authEpoch = AuthEpoch::from(1).value();
        if (scenario == 4) f.seed.sessions.erase(f.seed.sessions.begin());
        f.open();
        auto result = f.run(f.command());
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::Unauthenticated);
        BOOST_TEST(f.control.charges == 0);
        f.unchanged();
    }
}

BOOST_AUTO_TEST_CASE(expiry_or_identity_change_on_second_check_rolls_back_writes) {
    for (bool expiry : { false, true }) {
        Fixture f;
        if (expiry) f.seed.sessions[0].expiresAt = f.clock.second;
        else f.control.fault = Fault::ChangedIdentity;
        f.open();
        auto result = f.run(f.command());
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::Unauthenticated);
        BOOST_TEST(f.control.authorizations == 2);
        BOOST_TEST(f.control.charges == 2);
        BOOST_TEST(f.control.commits == 0);
        f.unchanged();
    }
}

BOOST_AUTO_TEST_CASE(validation_id_generation_and_begin_fail_before_writes) {
    for (int scenario = 0; scenario < 3; ++scenario) {
        Fixture f;
        auto command = f.command();
        if (scenario == 0) command.name = "../invalid";
        if (scenario == 1) f.ids.fail = true;
        if (scenario == 2) f.control.fault = Fault::Begin;
        f.open();
        auto result = f.run(command);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == (scenario == 0 ? ErrorCode::InvalidName :
                                           scenario == 1 ? ErrorCode::InternalError : ErrorCode::Retryable));
        BOOST_TEST(f.control.begins == (scenario == 2 ? 1 : 0));
        BOOST_TEST(f.ids.calls == (scenario == 0 ? 0 : 1));
        BOOST_CHECK(f.attempt.has_value() == (scenario == 2));
        f.unchanged();
    }
}

BOOST_AUTO_TEST_CASE(unavailable_or_file_parent_leaves_database_unchanged) {
    for (int scenario = 0; scenario < 5; ++scenario) {
        Fixture f;
        auto command = f.command();
        if (scenario == 0) command.spaceId = id<SpaceId>(99);
        if (scenario == 1) command.parentId = id<NodeId>(99);
        if (scenario == 2) command.parentId = f.privateRoot;
        if (scenario >= 3) {
            const auto node = id<NodeId>(3);
            std::optional<BlobId> blob;
            if (scenario == 3) {
                blob = id<BlobId>(1);
                f.seed.blobs.push_back({ *blob, ByteCount{}, {} });
            }
            f.seed.nodes.push_back({ { node, f.publicSpace, f.publicRoot, "parent", f.owner,
                                      Revision{}, f.clock.first, f.clock.first }, blob, scenario == 4 });
            command.parentId = node;
            if (scenario == 4) {
                const auto descendant = id<NodeId>(4);
                f.seed.nodes.push_back({ { descendant, f.publicSpace, node, "hidden", f.owner,
                                          Revision{}, f.clock.first, f.clock.first }, std::nullopt, false });
                command.parentId = descendant;
            }
        }
        f.open();
        auto result = f.run(command);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == (scenario == 3 ? ErrorCode::ParentNotDirectory : ErrorCode::NotFound));
        f.unchanged();
    }
}

BOOST_AUTO_TEST_CASE(quota_failure_rolls_back_all_scopes) {
    for (int scopeIndex : { 1, 2 }) {
        Fixture f;
        auto& quota = f.seed.quotas[static_cast<std::size_t>(scopeIndex)].state;
        quota.limit.nodes = NodeCount::from(quota.used.nodes.value() + 1).value();
        quota.reserved.nodes = NodeCount::from(1).value();
        f.open();
        auto result = f.run(f.command());
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::QuotaExceeded);
        f.unchanged();
    }
}

BOOST_AUTO_TEST_CASE(failures_after_mutations_and_exceptions_roll_back_everything) {
    for (auto fault : { Fault::FirstCharge, Fault::SecondCharge, Fault::Insert, Fault::Revision,
                        Fault::Audit, Fault::BeforeCommit, Fault::ThrowAfterAudit }) {
        Fixture f;
        f.control.fault = fault;
        f.open();
        if (fault == Fault::ThrowAfterAudit) {
            BOOST_CHECK_THROW(f.run(f.command()), std::runtime_error);
        } else {
            auto result = f.run(f.command());
            BOOST_REQUIRE(!result);
            BOOST_CHECK(result.error().code == (fault == Fault::BeforeCommit ? ErrorCode::Retryable : ErrorCode::InternalError));
        }
        BOOST_TEST(f.control.begins == 1);
        BOOST_TEST(f.ids.calls == 1);
        f.unchanged();
    }
}

BOOST_AUTO_TEST_CASE(unknown_commit_never_becomes_success_or_automatic_retry) {
    for (auto fault : { Fault::UnknownRolledBack, Fault::UnknownCommitted }) {
        Fixture f;
        f.control.fault = fault;
        f.open();
        auto result = f.run(f.command());
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::CommitOutcomeUnknown);
        BOOST_REQUIRE(f.attempt);
        BOOST_CHECK(f.attempt->candidateNodeId == f.ids.value);
        BOOST_CHECK(f.attempt->requestId == f.context().requestId);
        BOOST_TEST(f.control.commits == 1);
        BOOST_TEST(f.control.begins == 1);
        BOOST_TEST(f.ids.calls == 1);
        if (fault == Fault::UnknownRolledBack) f.unchanged();
        else {
            auto node = f.backend->readNode(f.ids.value);
            BOOST_REQUIRE(node);
            BOOST_CHECK(node.value().has_value());
            auto events = f.backend->readAudit();
            BOOST_REQUIRE(events);
            BOOST_TEST(events.value().size() == 1U);
        }
    }
}

BOOST_AUTO_TEST_CASE(name_conflict_does_not_charge_or_increment_revision) {
    for (bool file : { false, true }) {
        Fixture f;
        std::optional<BlobId> blob;
        if (file) {
            blob = id<BlobId>(1);
            f.seed.blobs.push_back({ *blob, ByteCount{}, {} });
        }
        f.seed.nodes.push_back({ { id<NodeId>(3), f.publicSpace, f.publicRoot, f.command().name, f.owner,
                                  Revision{}, f.clock.first, f.clock.first }, blob, false });
        f.open();
        auto result = f.run(f.command());
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::NameConflict);
        f.unchanged();
    }
}

BOOST_AUTO_TEST_CASE(depth_limit_and_revision_overflow_do_not_leave_partial_writes) {
    for (bool overflow : { false, true }) {
        Fixture f;
        auto command = f.command();
        if (overflow) {
            f.seed.nodes[1].metadata.revision = Revision::from(std::numeric_limits<std::int64_t>::max()).value();
        } else {
            auto parent = f.publicRoot;
            for (unsigned int i = 0; i < 128; ++i) {
                auto node = id<NodeId>(10 + i);
                f.seed.nodes.push_back({ { node, f.publicSpace, parent, "level", f.owner,
                                          Revision{}, f.clock.first, f.clock.first }, std::nullopt, false });
                parent = node;
            }
            command.parentId = parent;
        }
        f.open();
        auto result = f.run(command);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == (overflow ? ErrorCode::IntegrityError : ErrorCode::DepthLimitExceeded));
        f.unchanged();
    }
}

BOOST_AUTO_TEST_CASE(two_connections_compete_without_duplicate_or_overquota_writes) {
    // 同步交错两个独立连接，不创建压力负载；第二连接在第一个持写锁时进入用例。
    for (bool sameName : { false, true }) {
        Fixture f;
        if (!sameName) f.seed.quotas[1].state.limit.nodes = NodeCount::from(2).value();
        f.open();
        auto otherStore = f.backend->connect();
        BOOST_REQUIRE(otherStore);
        TestClock otherClock;
        TestIds otherIds;
        otherIds.value = id<NodeId>(1001);
        CreateDirectory other(*otherStore.value(), otherClock, otherIds);
        auto otherCommand = f.command();
        if (!sameName) otherCommand.name = "different";
        f.control.onFirstCharge = [&] {
            auto busy = other.execute(f.context(), otherCommand);
            BOOST_REQUIRE(!busy);
            BOOST_CHECK(busy.error().code == ErrorCode::Retryable);
            BOOST_TEST(otherIds.calls == 1);
        };
        auto winner = f.run(f.command());
        BOOST_REQUIRE(winner);
        // 测试明确获知未提交后手动重试，应用服务自身没有重试循环。
        auto loser = other.execute(f.context(), otherCommand);
        BOOST_REQUIRE(!loser);
        BOOST_CHECK(loser.error().code == (sameName ? ErrorCode::NameConflict : ErrorCode::QuotaExceeded));
        auto node = f.backend->readNode(otherIds.value);
        BOOST_REQUIRE(node);
        BOOST_CHECK(!node.value());
        auto usage = f.backend->readQuota(SpaceQuota{ f.publicSpace });
        BOOST_REQUIRE(usage);
        BOOST_TEST(usage.value().used.nodes.value() == 2);
        auto events = f.backend->readAudit();
        BOOST_REQUIRE(events);
        BOOST_TEST(events.value().size() == 1U);
    }
}

}  // namespace anof::testing
