#define BOOST_TEST_MODULE AnofMetadataContract
#include <boost/test/unit_test.hpp>

#include <tests/persistence_contract/TestMetadataBackend.h>

#include <chrono>
#include <iomanip>
#include <sstream>

namespace anof::testing {
namespace {

template <typename T>
T id(unsigned int value) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setfill('0') << std::setw(12) << value;
    return T::parse(text.str()).value();
}

QuotaDelta amount(std::int64_t bytes, std::int64_t nodes) {
    return { ByteCount::from(bytes).value(), NodeCount::from(nodes).value() };
}

struct Scenario {
    UserId user = id<UserId>(1);
    SpaceId space = id<SpaceId>(1);
    NodeId root = id<NodeId>(1);
    NodeId child = id<NodeId>(2);
    SessionDigest digest{ SessionDigest::Bytes{ 1 } };
    UtcTime now{ std::chrono::microseconds{ 1000000 } };
    QuotaScope scope = SpaceQuota{ space };
    MetadataSeed seed{
        { { { user, UserRole::Administrator, AuthEpoch{} }, true } },
        { { digest, user, AuthEpoch{}, now + std::chrono::hours(1), false } },
        { { space, SpaceKind::Public, std::nullopt, root } },
        {},
        { { { root, space, std::nullopt, "", user, Revision{}, now, now }, std::nullopt, false } },
        { { scope, { amount(1024, 10), amount(0, 1), amount(0, 0) } },
          { PublicContributionQuota{ space, user }, { amount(1024, 10), amount(0, 0), amount(0, 0) } } }
    };

    NewDirectory directory(std::string name = "child") const {
        return { child, space, root, std::move(name), user, now };
    }
    DirectoryCreatedEvent event() const {
        return { user, space, child, root, id<RequestId>(1), now };
    }
};

std::unique_ptr<TestMetadataBackend> backendFor(const MetadataSeed& seed) {
    auto backend = makeTestMetadataBackend();
    BOOST_REQUIRE(backend);
    BOOST_REQUIRE(backend->reset(seed));
    return backend;
}

std::unique_ptr<MetadataStore> connect(TestMetadataBackend& backend) {
    auto connection = backend.connect();
    BOOST_REQUIRE(connection);
    BOOST_REQUIRE(connection.value());
    return std::move(connection).value();
}

std::unique_ptr<MetadataTransaction> begin(MetadataStore& store) {
    auto transaction = store.beginWrite();
    BOOST_REQUIRE(transaction);
    BOOST_REQUIRE(transaction.value());
    return std::move(transaction).value();
}

void stageCreation(MetadataTransaction& tx, const Scenario& s) {
    BOOST_REQUIRE(tx.quotas().chargeUsed(s.scope, amount(0, 1)));
    BOOST_REQUIRE(tx.quotas().chargeUsed(PublicContributionQuota{ s.space, s.user }, amount(0, 1)));
    auto inserted = tx.nodes().insertDirectory(s.directory());
    BOOST_REQUIRE(inserted);
    BOOST_CHECK(inserted.value().id == s.child);
    BOOST_TEST(inserted.value().revision.value() == 1);
    BOOST_CHECK(inserted.value().createdAt == s.now);
    BOOST_CHECK(inserted.value().updatedAt == s.now);
    auto revision = tx.nodes().advanceRevision(s.root, Revision{}, s.now);
    BOOST_REQUIRE(revision);
    BOOST_TEST(revision.value().value() == 2);
    BOOST_REQUIRE(tx.audit().append(s.event()));
}

void checkCreation(TestMetadataBackend& backend, const Scenario& s, bool committed) {
    auto child = backend.readNode(s.child);
    BOOST_REQUIRE(child);
    BOOST_CHECK(child.value().has_value() == committed);
    auto root = backend.readNode(s.root);
    BOOST_REQUIRE(root);
    BOOST_REQUIRE(root.value());
    BOOST_TEST(root.value()->metadata.revision.value() == (committed ? 2 : 1));
    auto usage = backend.readQuota(s.scope);
    BOOST_REQUIRE(usage);
    BOOST_TEST(usage.value().used.nodes.value() == (committed ? 2 : 1));
    auto contribution = backend.readQuota(PublicContributionQuota{ s.space, s.user });
    BOOST_REQUIRE(contribution);
    BOOST_TEST(contribution.value().used.nodes.value() == (committed ? 1 : 0));
    auto events = backend.readAudit();
    BOOST_REQUIRE(events);
    BOOST_TEST(events.value().size() == (committed ? 1U : 0U));
    if (committed) {
        BOOST_CHECK(events.value()[0].nodeId == s.child);
        BOOST_CHECK(events.value()[0].actor == s.user);
        BOOST_CHECK(events.value()[0].requestId == s.event().requestId);
    }
}

}  // namespace

BOOST_AUTO_TEST_CASE(commit_persists_all_repositories_after_reconnect) {
    const Scenario s;
    auto backend = backendFor(s.seed);
    {
        auto store = connect(*backend);
        auto tx = begin(*store);
        stageCreation(*tx, s);
        BOOST_REQUIRE(tx->commit());
    }
    checkCreation(*backend, s, true);
    auto store = connect(*backend);
    auto tx = begin(*store);
    auto path = tx->nodes().findActiveDirectoryPath(s.space, s.child);
    BOOST_REQUIRE(path);
    BOOST_REQUIRE_EQUAL(path.value().size(), 2U);
    BOOST_CHECK(path.value().front().id == s.root);
    BOOST_CHECK(path.value().back().id == s.child);
}

BOOST_AUTO_TEST_CASE(destructor_rolls_back_each_partial_stage) {
    const Scenario s;
    for (int stage = 0; stage < 5; ++stage) {
        auto backend = backendFor(s.seed);
        {
            auto store = connect(*backend);
            auto tx = begin(*store);
            BOOST_REQUIRE(tx->quotas().chargeUsed(s.scope, amount(0, 1)));
            if (stage >= 1) BOOST_REQUIRE(tx->quotas().chargeUsed(PublicContributionQuota{ s.space, s.user }, amount(0, 1)));
            if (stage >= 2) BOOST_REQUIRE(tx->nodes().insertDirectory(s.directory()));
            if (stage >= 3) BOOST_REQUIRE(tx->nodes().advanceRevision(s.root, Revision{}, s.now));
            if (stage >= 4) BOOST_REQUIRE(tx->audit().append(s.event()));
        }
        checkCreation(*backend, s, false);
    }
}

BOOST_AUTO_TEST_CASE(active_names_are_byte_unique_and_share_file_namespace) {
    for (bool file : { false, true }) {
        Scenario s;
        std::optional<BlobId> blob;
        if (file) {
            blob = id<BlobId>(1);
            s.seed.blobs.push_back({ *blob, ByteCount{}, {} });
        }
        s.seed.nodes.push_back({ { s.child, s.space, s.root, "child", s.user, Revision{}, s.now, s.now }, blob, false });
        auto backend = backendFor(s.seed);
        auto store = connect(*backend);
        auto tx = begin(*store);
        auto duplicate = s.directory();
        duplicate.id = id<NodeId>(3);
        auto result = tx->nodes().insertDirectory(duplicate);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::NameConflict);
        // 失败操作之后结束事务，使用新事务验证字节大小写规则。
        tx.reset();
        tx = begin(*store);
        duplicate.name = "Child";
        BOOST_REQUIRE(tx->nodes().insertDirectory(duplicate));
    }
}

BOOST_AUTO_TEST_CASE(trashed_ancestor_and_non_directory_are_rejected) {
    for (bool file : { false, true }) {
        Scenario s;
        std::optional<BlobId> blob;
        if (file) {
            blob = id<BlobId>(1);
            s.seed.blobs.push_back({ *blob, ByteCount{}, {} });
        }
        s.seed.nodes.push_back({ { s.child, s.space, s.root, "child", s.user, Revision{}, s.now, s.now }, blob, !file });
        const auto descendant = id<NodeId>(3);
        if (!file) s.seed.nodes.push_back({ { descendant, s.space, s.child, "descendant", s.user, Revision{}, s.now, s.now }, std::nullopt, false });
        auto backend = backendFor(s.seed);
        auto store = connect(*backend);
        auto tx = begin(*store);
        auto path = tx->nodes().findActiveDirectoryPath(s.space, file ? s.child : descendant);
        BOOST_REQUIRE(!path);
        BOOST_CHECK(path.error().code == (file ? ErrorCode::ParentNotDirectory : ErrorCode::NotFound));
        tx.reset();
        if (!file) {
            tx = begin(*store);
            auto newNode = s.directory();
            newNode.id = id<NodeId>(4);
            BOOST_REQUIRE(tx->nodes().insertDirectory(newNode));
        }
    }
}

BOOST_AUTO_TEST_CASE(auth_state_is_checked_inside_transaction) {
    for (int invalid = 0; invalid < 6; ++invalid) {
        Scenario s;
        if (invalid == 1) s.seed.sessions[0].expiresAt = s.now;
        if (invalid == 2) s.seed.sessions[0].revoked = true;
        if (invalid == 3) s.seed.users[0].enabled = false;
        if (invalid == 4) s.seed.users[0].principal.authEpoch = AuthEpoch::from(1).value();
        if (invalid == 5) s.seed.sessions.clear();
        auto backend = backendFor(s.seed);
        auto store = connect(*backend);
        auto tx = begin(*store);
        auto principal = tx->authorization().requireActiveSession(s.digest, s.now);
        if (invalid == 0) {
            BOOST_REQUIRE(principal);
            BOOST_CHECK(principal.value().userId == s.user);
        } else {
            BOOST_REQUIRE(!principal);
            BOOST_CHECK(principal.error().code == ErrorCode::Unauthenticated);
        }
    }
}

BOOST_AUTO_TEST_CASE(reserved_and_second_scope_failure_preserve_usage) {
    Scenario s;
    s.seed.quotas[1].state.limit.nodes = NodeCount::from(1).value();
    s.seed.quotas[1].state.reserved.nodes = NodeCount::from(1).value();
    auto backend = backendFor(s.seed);
    {
        auto store = connect(*backend);
        auto tx = begin(*store);
        BOOST_REQUIRE(tx->quotas().chargeUsed(s.scope, amount(0, 1)));
        auto denied = tx->quotas().chargeUsed(PublicContributionQuota{ s.space, s.user }, amount(0, 1));
        BOOST_REQUIRE(!denied);
        BOOST_CHECK(denied.error().code == ErrorCode::QuotaExceeded);
    }
    checkCreation(*backend, s, false);
    auto usage = backend->readQuota(PublicContributionQuota{ s.space, s.user });
    BOOST_REQUIRE(usage);
    BOOST_TEST(usage.value().reserved.nodes.value() == 1);
}

BOOST_AUTO_TEST_CASE(revision_compare_and_nested_transaction_rejection) {
    const Scenario s;
    auto backend = backendFor(s.seed);
    auto store = connect(*backend);
    auto tx = begin(*store);
    auto nested = store->beginWrite();
    BOOST_REQUIRE(!nested);
    BOOST_CHECK(nested.error().code == ErrorCode::InvalidArgument);
    auto stale = tx->nodes().advanceRevision(s.root, Revision::from(2).value(), s.now);
    BOOST_REQUIRE(!stale);
    BOOST_CHECK(stale.error().code == ErrorCode::Conflict);
    tx.reset();
    checkCreation(*backend, s, false);
}

}  // namespace anof::testing
