#define BOOST_TEST_MODULE AnofSqliteQuotaAudit
#include <boost/test/unit_test.hpp>

#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestMetadataBackend.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace anof::infrastructure::sqlite::testing {
namespace {

using anof::testing::MetadataSeed;

template <typename T>
T id(unsigned value) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setfill('0') << std::setw(12) << value;
    return T::parse(text.str()).value();
}

QuotaDelta amount(std::int64_t bytes, std::int64_t nodes) {
    return { ByteCount::from(bytes).value(), NodeCount::from(nodes).value() };
}

template <typename T>
void errorIs(const Result<T>& result, ErrorCode expected) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == expected);
}

struct Scenario {
    UserId user = id<UserId>(1);
    SpaceId space = id<SpaceId>(1);
    NodeId root = id<NodeId>(1);
    NodeId child = id<NodeId>(2);
    UtcTime now{ std::chrono::microseconds{ 1234567 } };
    QuotaScope scope = SpaceQuota{ space };
    QuotaScope contribution = PublicContributionQuota{ space, user };
    MetadataSeed seed{
        { { { user, UserRole::Administrator, AuthEpoch{} }, true } }, {},
        { { space, SpaceKind::Public, std::nullopt, root } }, {},
        { { { root, space, std::nullopt, "", user, Revision{}, now, now }, std::nullopt, false } },
        { { scope, { amount(100, 10), amount(10, 1), amount(20, 2) } },
          { contribution, { amount(100, 10), amount(10, 1), amount(20, 2) } } }
    };
    NewDirectory directory() const { return { child, space, root, "child", user, now }; }
    DirectoryCreatedEvent event() const { return { user, space, child, root, id<RequestId>(10), now }; }
};

// 同库覆盖公共空间、管理员贡献、普通成员贡献及私有空间，观察相互隔离。
Scenario accountQuotas() {
    Scenario s;
    const auto member = id<UserId>(2);
    const auto personal = id<SpaceId>(2);
    const auto root = id<NodeId>(3);
    s.seed.users.push_back({ { member, UserRole::Member, AuthEpoch{} }, true });
    s.seed.spaces.push_back({ personal, SpaceKind::Private, member, root });
    s.seed.nodes.push_back({ { root, personal, std::nullopt, "", member, Revision{}, s.now, s.now }, std::nullopt, false });
    const auto state = s.seed.quotas.front().state;
    s.seed.quotas.push_back({ PublicContributionQuota{ s.space, member }, state });
    s.seed.quotas.push_back({ SpaceQuota{ personal }, state });
    return s;
}

void checkQuota(const QuotaSnapshot& actual, const QuotaState& expected, std::int64_t revision) {
    BOOST_CHECK_EQUAL(actual.state.limit.bytes.value(), expected.limit.bytes.value());
    BOOST_CHECK_EQUAL(actual.state.limit.nodes.value(), expected.limit.nodes.value());
    BOOST_CHECK_EQUAL(actual.state.used.bytes.value(), expected.used.bytes.value());
    BOOST_CHECK_EQUAL(actual.state.used.nodes.value(), expected.used.nodes.value());
    BOOST_CHECK_EQUAL(actual.state.reserved.bytes.value(), expected.reserved.bytes.value());
    BOOST_CHECK_EQUAL(actual.state.reserved.nodes.value(), expected.reserved.nodes.value());
    BOOST_CHECK_EQUAL(actual.revision.value(), revision);
}

std::unique_ptr<MetadataStore> connect(SqliteTestMetadataBackend& backend) {
    auto result = backend.connect();
    BOOST_REQUIRE(result);
    return std::move(result).value();
}

std::unique_ptr<MetadataTransaction> begin(MetadataStore& store) {
    auto result = store.beginWrite();
    BOOST_REQUIRE(result);
    return std::move(result).value();
}

void execute(SqliteTestMetadataBackend& backend, const std::string& sql) {
    auto connection = openSqliteDatabase(backend.databasePath(), { 0 });
    BOOST_REQUIRE(connection);
    BOOST_REQUIRE(connection.value()->execute(sql));
}

void stage(MetadataTransaction& transaction, const Scenario& s) {
    BOOST_REQUIRE(transaction.quotas().chargeUsed(s.scope, amount(0, 1)));
    BOOST_REQUIRE(transaction.quotas().chargeUsed(s.contribution, amount(0, 1)));
    BOOST_REQUIRE(transaction.nodes().insertDirectory(s.directory()));
    BOOST_REQUIRE(transaction.nodes().advanceRevision(s.root, Revision{}, s.now));
}

void checkRolledBack(SqliteTestMetadataBackend& backend, const Scenario& s) {
    auto node = backend.readNode(s.child);
    BOOST_REQUIRE(node);
    BOOST_CHECK(!node.value());
    auto root = backend.readNode(s.root);
    BOOST_REQUIRE(root && root.value());
    BOOST_TEST(root.value()->metadata.revision.value() == 1);
    for (const auto& scope : { s.scope, s.contribution }) {
        auto quota = backend.readQuota(scope);
        BOOST_REQUIRE(quota);
        BOOST_TEST(quota.value().used.bytes.value() == 10);
        BOOST_TEST(quota.value().used.nodes.value() == 1);
    }
    auto events = backend.readAudit();
    BOOST_REQUIRE(events);
    BOOST_CHECK(events.value().empty());
}

}  // namespace

BOOST_AUTO_TEST_CASE(both_quota_scopes_charge_bytes_and_nodes_without_changing_reservations) {
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto store = connect(backend);
    auto tx = begin(*store);
    for (const auto& scope : { s.scope, s.contribution }) {
        BOOST_REQUIRE(tx->quotas().chargeUsed(scope, amount(0, 0)));
        BOOST_REQUIRE(tx->quotas().chargeUsed(scope, amount(70, 7)));
    }
    BOOST_REQUIRE(tx->commit());
    for (const auto& scope : { s.scope, s.contribution }) {
        auto quota = backend.readQuota(scope);
        BOOST_REQUIRE(quota);
        BOOST_TEST(quota.value().used.bytes.value() == 80);
        BOOST_TEST(quota.value().used.nodes.value() == 8);
        BOOST_TEST(quota.value().reserved.bytes.value() == 20);
        BOOST_TEST(quota.value().reserved.nodes.value() == 2);
        BOOST_TEST(quota.value().limit.bytes.value() == 100);
    }
}

BOOST_AUTO_TEST_CASE(private_space_charging_needs_no_public_contribution_row) {
    Scenario s;
    s.seed.spaces[0].kind = SpaceKind::Private;
    s.seed.spaces[0].ownerId = s.user;
    s.seed.quotas.erase(s.seed.quotas.begin() + 1, s.seed.quotas.end());
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto store = connect(backend);
    auto tx = begin(*store);
    BOOST_REQUIRE(tx->quotas().chargeUsed(s.scope, amount(0, 1)));
    BOOST_REQUIRE(tx->commit());
    auto quota = backend.readQuota(s.scope);
    BOOST_REQUIRE(quota);
    BOOST_TEST(quota.value().used.nodes.value() == 2);
    auto events = backend.readAudit();
    BOOST_REQUIRE(events);
    BOOST_CHECK(events.value().empty());
}

BOOST_AUTO_TEST_CASE(either_dimension_exceeding_quota_does_not_partially_charge) {
    for (const auto delta : { amount(71, 0), amount(1, 8) }) {
        Scenario s;
        SqliteTestMetadataBackend backend;
        BOOST_REQUIRE(backend.reset(s.seed));
        auto store = connect(backend);
        auto tx = begin(*store);
        errorIs(tx->quotas().chargeUsed(s.scope, delta), ErrorCode::QuotaExceeded);
        errorIs(tx->commit(), ErrorCode::InternalError);
        checkRolledBack(backend, s);
    }
}

BOOST_AUTO_TEST_CASE(quota_int64_boundaries_are_checked_without_content_files) {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const std::int64_t boundary : { 2147483648LL, 4294967296LL, 1099511627776LL, 9223372036854775807LL }) {
        Scenario s;
        s.seed.quotas[0].state = { amount(maximum, maximum), amount(boundary - 1, boundary - 1), amount(0, 0) };
        SqliteTestMetadataBackend backend;
        BOOST_REQUIRE(backend.reset(s.seed));
        auto store = connect(backend);
        auto tx = begin(*store);
        BOOST_REQUIRE(tx->quotas().chargeUsed(s.scope, amount(1, 1)));
        BOOST_REQUIRE(tx->commit());
        auto quota = backend.readQuota(s.scope);
        BOOST_REQUIRE(quota);
        BOOST_TEST(quota.value().used.bytes.value() == boundary);
        BOOST_TEST(quota.value().used.nodes.value() == boundary);
        if (boundary == maximum) {
            for (const auto delta : { amount(1, 0), amount(0, 1) }) {
                tx = begin(*store);
                errorIs(tx->quotas().chargeUsed(s.scope, delta), ErrorCode::IntegrityError);
                errorIs(tx->commit(), ErrorCode::InternalError);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(missing_and_corrupt_quota_rows_are_integrity_errors) {
    const char* corruptions[] = {
        "DELETE FROM space_quotas",
        "PRAGMA ignore_check_constraints=ON; UPDATE space_quotas SET used_bytes=-1",
        "PRAGMA ignore_check_constraints=ON; UPDATE space_quotas SET reserved_nodes=99",
        "PRAGMA ignore_check_constraints=ON; UPDATE space_quotas SET used_bytes=9223372036854775807, reserved_bytes=1",
        "PRAGMA ignore_check_constraints=ON; UPDATE space_quotas SET used_nodes='invalid'"
    };
    for (const auto* sql : corruptions) {
        Scenario s;
        SqliteTestMetadataBackend backend;
        BOOST_REQUIRE(backend.reset(s.seed));
        execute(backend, sql);
        auto store = connect(backend);
        auto tx = begin(*store);
        errorIs(tx->quotas().chargeUsed(s.scope, amount(0, 1)), ErrorCode::IntegrityError);
        errorIs(tx->commit(), ErrorCode::InternalError);
    }
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto store = connect(backend);
    auto tx = begin(*store);
    errorIs(tx->quotas().chargeUsed(PublicContributionQuota{ s.space, id<UserId>(99) }, amount(0, 1)), ErrorCode::IntegrityError);
}

BOOST_AUTO_TEST_CASE(account_quota_limits_preserve_counters_and_isolate_each_scope) {
    const auto s = accountQuotas();
    for (const auto& target : s.seed.quotas) {
        SqliteTestMetadataBackend backend;
        BOOST_REQUIRE(backend.reset(s.seed));
        auto store = connect(backend);
        {
            auto tx = begin(*store);
            auto same = tx->quotas().setLimit(target.scope, Revision{}, target.state.limit);
            BOOST_REQUIRE(same);
            checkQuota(same.value(), target.state, 1);
            auto lowered = tx->quotas().setLimit(target.scope, Revision{}, amount(30, 3));
            BOOST_REQUIRE(lowered);
            checkQuota(lowered.value(), { amount(30, 3), target.state.used, target.state.reserved }, 2);
            auto unchanged = tx->quotas().setLimit(target.scope, Revision::from(2).value(), amount(30, 3));
            BOOST_REQUIRE(unchanged);
            checkQuota(unchanged.value(), lowered.value().state, 2);
            BOOST_REQUIRE(tx->commit());
        }
        // 过期版本优先于无变化及额度下限；两个维度分别验证，避免其中一个掩盖另一个。
        for (auto limit : { amount(30, 3), amount(29, 3), amount(30, 2) }) {
            auto tx = begin(*store);
            errorIs(tx->quotas().setLimit(target.scope, Revision{}, limit), ErrorCode::PreconditionFailed);
            BOOST_CHECK(!tx->commit());
        }
        for (auto limit : { amount(29, 3), amount(30, 2) }) {
            auto tx = begin(*store);
            errorIs(tx->quotas().setLimit(target.scope, Revision::from(2).value(), limit), ErrorCode::QuotaExceeded);
            BOOST_CHECK(!tx->commit());
        }
        auto read = store->beginRead();
        BOOST_REQUIRE(read);
        for (const auto& entry : s.seed.quotas) {
            auto quota = read.value()->quotas().get(entry.scope);
            BOOST_REQUIRE(quota);
            const bool changed = entry.scope == target.scope;
            checkQuota(quota.value(), { changed ? amount(30, 3) : entry.state.limit, entry.state.used, entry.state.reserved }, changed ? 2 : 1);
        }
    }
}

BOOST_AUTO_TEST_CASE(account_quota_limits_support_zero_and_int64_maximum) {
    auto s = accountQuotas();
    // 仅验证仓储数值边界；零计数种子不代表完整实例初始化状态。
    for (auto& entry : s.seed.quotas) entry.state = { amount(100, 10), amount(0, 0), amount(0, 0) };
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto store = connect(backend);
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto& entry : s.seed.quotas) {
        auto tx = begin(*store);
        auto zero = tx->quotas().setLimit(entry.scope, Revision{}, amount(0, 0));
        BOOST_REQUIRE(zero);
        checkQuota(zero.value(), { amount(0, 0), amount(0, 0), amount(0, 0) }, 2);
        BOOST_REQUIRE(tx->commit());
        tx = begin(*store);
        auto raised = tx->quotas().setLimit(entry.scope, Revision::from(2).value(), amount(maximum, maximum));
        BOOST_REQUIRE(raised);
        checkQuota(raised.value(), { amount(maximum, maximum), amount(0, 0), amount(0, 0) }, 3);
        BOOST_REQUIRE(tx->commit());
    }
    auto read = store->beginRead();
    BOOST_REQUIRE(read);
    for (const auto& entry : s.seed.quotas) {
        auto quota = read.value()->quotas().get(entry.scope);
        BOOST_REQUIRE(quota);
        checkQuota(quota.value(), { amount(maximum, maximum), amount(0, 0), amount(0, 0) }, 3);
    }
}

BOOST_AUTO_TEST_CASE(account_quota_revision_overflow_allows_noop_but_rejects_changes) {
    const auto s = accountQuotas();
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    execute(backend, "UPDATE space_quotas SET revision=9223372036854775807; UPDATE public_contribution_quotas SET revision=9223372036854775807");
    auto store = connect(backend);
    const auto maximum = Revision::from(std::numeric_limits<std::int64_t>::max()).value();
    for (const auto& entry : s.seed.quotas) {
        auto tx = begin(*store);
        auto same = tx->quotas().setLimit(entry.scope, maximum, entry.state.limit);
        BOOST_REQUIRE(same);
        checkQuota(same.value(), entry.state, maximum.value());
        BOOST_REQUIRE(tx->commit());
        tx = begin(*store);
        errorIs(tx->quotas().setLimit(entry.scope, maximum, amount(200, 20)), ErrorCode::IntegrityError);
        BOOST_CHECK(!tx->commit());
    }
    auto read = store->beginRead();
    BOOST_REQUIRE(read);
    for (const auto& entry : s.seed.quotas) {
        auto quota = read.value()->quotas().get(entry.scope);
        BOOST_REQUIRE(quota);
        checkQuota(quota.value(), entry.state, maximum.value());
    }
}

BOOST_AUTO_TEST_CASE(account_quota_missing_rows_and_occupied_overflow_fail_closed) {
    const auto s = accountQuotas();
    for (const auto& target : s.seed.quotas) {
        const char* table = std::holds_alternative<SpaceQuota>(target.scope) ? "space_quotas" : "public_contribution_quotas";
        for (unsigned damage = 0; damage != 3; ++damage) {
            SqliteTestMetadataBackend backend;
            BOOST_REQUIRE(backend.reset(s.seed));
            if (damage == 0) {
                execute(backend, std::string("DELETE FROM ") + table);
            } else {
                const std::string dimension = damage == 1 ? "bytes" : "nodes";
                execute(backend, std::string("PRAGMA ignore_check_constraints=ON; UPDATE ") + table +
                    " SET limit_" + dimension + "=9223372036854775807,used_" + dimension +
                    "=9223372036854775807,reserved_" + dimension + "=1");
            }
            auto store = connect(backend);
            auto tx = begin(*store);
            // 损坏状态必须报 IntegrityError，不能被目标 limit 或旧 revision 掩盖。
            errorIs(tx->quotas().setLimit(target.scope, Revision::from(2).value(), amount(0, 0)), ErrorCode::IntegrityError);
            BOOST_CHECK(!tx->commit());
            tx = begin(*store);
            errorIs(tx->quotas().get(target.scope), ErrorCode::IntegrityError);
        }
    }
}

BOOST_AUTO_TEST_CASE(charge_before_limit_update_invalidates_expected_revision) {
    const auto s = accountQuotas();
    for (const auto& target : s.seed.quotas) {
        SqliteTestMetadataBackend backend;
        BOOST_REQUIRE(backend.reset(s.seed));
        auto charging = connect(backend);
        auto managing = connect(backend);
        auto tx = begin(*charging);
        BOOST_REQUIRE(tx->quotas().chargeUsed(target.scope, amount(1, 1)));
        // 两个连接的写事务真实重叠；busy_timeout=0，不依赖 sleep 或线程调度。
        errorIs(managing->beginWrite(), ErrorCode::Retryable);
        BOOST_REQUIRE(tx->commit());
        auto update = begin(*managing);
        errorIs(update->quotas().setLimit(target.scope, Revision{}, amount(30, 3)), ErrorCode::PreconditionFailed);
        BOOST_CHECK(!update->commit());
        update = begin(*managing);
        errorIs(update->quotas().setLimit(target.scope, Revision::from(2).value(), amount(30, 3)), ErrorCode::QuotaExceeded);
        BOOST_CHECK(!update->commit());
        auto read = managing->beginRead();
        BOOST_REQUIRE(read);
        auto stored = read.value()->quotas().get(target.scope);
        BOOST_REQUIRE(stored);
        checkQuota(stored.value(), { target.state.limit, amount(11, 2), target.state.reserved }, 2);
    }
}

BOOST_AUTO_TEST_CASE(limit_update_before_charge_enforces_current_limit_in_both_dimensions) {
    const auto s = accountQuotas();
    for (const auto& target : s.seed.quotas) {
        for (const auto delta : { amount(1, 0), amount(0, 1) }) {
            SqliteTestMetadataBackend backend;
            BOOST_REQUIRE(backend.reset(s.seed));
            auto managing = connect(backend);
            auto charging = connect(backend);
            auto update = begin(*managing);
            BOOST_REQUIRE(update->quotas().setLimit(target.scope, Revision{}, amount(30, 3)));
            errorIs(charging->beginWrite(), ErrorCode::Retryable);
            BOOST_REQUIRE(update->commit());
            auto tx = begin(*charging);
            errorIs(tx->quotas().chargeUsed(target.scope, delta), ErrorCode::QuotaExceeded);
            BOOST_CHECK(!tx->commit());
            // 上调后同一计费能够成功，证明按当前已提交额度重新读取。
            update = begin(*managing);
            BOOST_REQUIRE(update->quotas().setLimit(target.scope, Revision::from(2).value(), amount(31, 4)));
            BOOST_REQUIRE(update->commit());
            tx = begin(*charging);
            BOOST_REQUIRE(tx->quotas().chargeUsed(target.scope, delta));
            BOOST_REQUIRE(tx->commit());
            auto read = managing->beginRead();
            BOOST_REQUIRE(read);
            auto stored = read.value()->quotas().get(target.scope);
            BOOST_REQUIRE(stored);
            checkQuota(stored.value(), { amount(31, 4), amount(10 + delta.bytes.value(), 1 + delta.nodes.value()), target.state.reserved }, 4);
        }
    }
}

BOOST_AUTO_TEST_CASE(audit_preserves_fields_and_request_id_is_not_an_idempotency_key) {
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto store = connect(backend);
    auto tx = begin(*store);
    auto first = s.event();
    auto second = first;
    second.nodeId = id<NodeId>(3);
    second.occurredAt -= std::chrono::microseconds(1);
    BOOST_REQUIRE(tx->directoryAudit().append(first));
    BOOST_REQUIRE(tx->directoryAudit().append(second));
    BOOST_REQUIRE(tx->commit());
    auto events = backend.readAudit();
    BOOST_REQUIRE(events);
    BOOST_REQUIRE_EQUAL(events.value().size(), 2U);
    const auto& actual = events.value()[0];
    BOOST_CHECK(actual.actor == first.actor);
    BOOST_CHECK(actual.spaceId == first.spaceId);
    BOOST_CHECK(actual.nodeId == first.nodeId);
    BOOST_CHECK(actual.parentId == first.parentId);
    BOOST_CHECK(actual.requestId == first.requestId);
    BOOST_CHECK(actual.occurredAt == first.occurredAt);
    BOOST_CHECK(events.value()[1].nodeId == second.nodeId);
    BOOST_CHECK(events.value()[1].occurredAt == second.occurredAt);
}

BOOST_AUTO_TEST_CASE(audit_failure_rolls_back_prior_charges_node_and_revision) {
    for (int failure = 0; failure != 3; ++failure) {
        Scenario s;
        SqliteTestMetadataBackend backend;
        BOOST_REQUIRE(backend.reset(s.seed));
        if (failure == 1) execute(backend, "CREATE TRIGGER test_audit_failure BEFORE INSERT ON audit_events BEGIN SELECT RAISE(ABORT, 'injected audit failure'); END");
        if (failure == 2) execute(backend, "CREATE TRIGGER test_audit_ignore BEFORE INSERT ON audit_events BEGIN SELECT RAISE(IGNORE); END");
        auto store = connect(backend);
        auto tx = begin(*store);
        stage(*tx, s);
        auto event = s.event();
        if (failure == 0) event.actor = id<UserId>(99);
        errorIs(tx->directoryAudit().append(event), ErrorCode::IntegrityError);
        errorIs(tx->commit(), ErrorCode::InternalError);
        checkRolledBack(backend, s);
    }
}

BOOST_AUTO_TEST_CASE(ignored_quota_update_cannot_report_a_charge) {
    Scenario s;
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    execute(backend, "CREATE TRIGGER test_quota_ignore BEFORE UPDATE ON space_quotas BEGIN SELECT RAISE(IGNORE); END");
    auto store = connect(backend);
    auto tx = begin(*store);
    errorIs(tx->quotas().chargeUsed(s.scope, amount(0, 1)), ErrorCode::IntegrityError);
    errorIs(tx->commit(), ErrorCode::InternalError);
    checkRolledBack(backend, s);
}

BOOST_AUTO_TEST_CASE(two_connections_cannot_spend_the_last_node_twice) {
    Scenario s;
    s.seed.quotas[0].state = { amount(100, 2), amount(10, 1), amount(0, 0) };
    SqliteTestMetadataBackend backend;
    BOOST_REQUIRE(backend.reset(s.seed));
    auto first = connect(backend);
    auto second = connect(backend);
    auto winner = begin(*first);
    stage(*winner, s);
    BOOST_REQUIRE(winner->directoryAudit().append(s.event()));
    errorIs(second->beginWrite(), ErrorCode::Retryable);
    BOOST_REQUIRE(winner->commit());
    auto loser = begin(*second);
    errorIs(loser->quotas().chargeUsed(s.scope, amount(0, 1)), ErrorCode::QuotaExceeded);
    errorIs(loser->commit(), ErrorCode::InternalError);
    auto quota = backend.readQuota(s.scope);
    BOOST_REQUIRE(quota);
    BOOST_TEST(quota.value().used.nodes.value() == 2);
    auto events = backend.readAudit();
    BOOST_REQUIRE(events);
    BOOST_TEST(events.value().size() == 1U);
    auto child = backend.readNode(s.child);
    BOOST_REQUIRE(child && child.value());
}

BOOST_AUTO_TEST_CASE(fixture_resets_isolates_and_round_trips_file_and_trashed_metadata) {
    Scenario s;
    const auto blob = id<BlobId>(1);
    s.seed.blobs.push_back({ blob, ByteCount::from(1099511627776LL).value(), {} });
    s.seed.nodes.push_back({ { s.child, s.space, s.root, "file", s.user, Revision::from(7).value(), s.now, s.now }, blob, true });
    std::reverse(s.seed.nodes.begin(), s.seed.nodes.end());
    std::filesystem::path savedPath;
    {
        SqliteTestMetadataBackend first;
        SqliteTestMetadataBackend second;
        savedPath = first.databasePath();
        BOOST_REQUIRE(first.reset(s.seed));
        Scenario empty;
        BOOST_REQUIRE(second.reset(empty.seed));
        auto node = first.readNode(s.child);
        BOOST_REQUIRE(node && node.value());
        BOOST_CHECK(node.value()->blobId.value() == blob);
        BOOST_CHECK(node.value()->trashed);
        BOOST_TEST(node.value()->metadata.revision.value() == 7);
        auto missing = second.readNode(s.child);
        BOOST_REQUIRE(missing);
        BOOST_CHECK(!missing.value());
        BOOST_REQUIRE(first.reset(empty.seed));
        missing = first.readNode(s.child);
        BOOST_REQUIRE(missing);
        BOOST_CHECK(!missing.value());
    }
    BOOST_CHECK(!std::filesystem::exists(savedPath.parent_path().parent_path()));
}

BOOST_AUTO_TEST_CASE(invalid_seed_rolls_back_earlier_rows) {
    Scenario s;
    s.seed.quotas[0].state.used.nodes = NodeCount::from(99).value();
    SqliteTestMetadataBackend backend;
    errorIs(backend.reset(s.seed), ErrorCode::IntegrityError);
    auto missing = backend.readNode(s.root);
    BOOST_REQUIRE(missing);
    BOOST_CHECK(!missing.value());
    auto connection = openSqliteDatabase(backend.databasePath(), { 0 });
    BOOST_REQUIRE(connection);
    auto statement = SqliteStatement::prepare(*connection.value(), "SELECT count(*) FROM users");
    BOOST_REQUIRE(statement);
    BOOST_REQUIRE(statement.value()->step());
    BOOST_TEST(statement.value()->integer(0).value() == 0);
}

}  // namespace anof::infrastructure::sqlite::testing
