#include <anof/application/ports/DriveMetadataStore.h>
#define BOOST_TEST_MODULE AnofSqliteReadSnapshot
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataReadSnapshot.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestMetadataBackend.h>
#include <sqlite3.h>

#include <boost/test/unit_test.hpp>
#include <cstdio>
#include <limits>

namespace anof::infrastructure::sqlite::testing {
namespace {
template <typename T>
T id(unsigned value) {
    char text[37];
    std::snprintf(text, sizeof(text), "00000000-0000-4000-8000-%012x", value);
    return T::parse(text).value();
}
template <typename T>
void errorIs(const Result<T>& result, ErrorCode code) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
}
QuotaDelta amount(std::int64_t bytes, std::int64_t nodes) {
    return { ByteCount::from(bytes).value(), NodeCount::from(nodes).value() };
}
const UtcTime now{ std::chrono::microseconds{ 123 } };
constexpr PasswordHashLimits budget{ 8192, 1, 1 };
NewUser user(unsigned n, UserRole role = UserRole::Member) {
    auto hash = EncodedPasswordHash::parse("$argon2id$v=19$m=8192,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", budget);
    return { { id<UserId>(n), LoginName::parse("user" + std::to_string(n)).value(), DisplayName::parse("User").value(), role, true,
               AuthEpoch{}, Revision{}, now, now },
             std::move(hash).value(),
             PasswordVersion{} };
}
struct Fixture {
    SqliteTestMetadataBackend backend;
    std::unique_ptr<MetadataStore> store;
    Fixture() {
        BOOST_REQUIRE(backend.reset({}));
        auto opened = backend.connect();
        BOOST_REQUIRE(opened);
        store = std::move(opened).value();
        auto tx = write();
        BOOST_REQUIRE(tx->users().insert(user(1, UserRole::Administrator)));
        for (unsigned n : { 1u, 2u }) {
            BOOST_REQUIRE(tx->spaces().insert({ id<SpaceId>(n), n == 1 ? SpaceKind::Private : SpaceKind::Public,
                                                n == 1 ? std::optional<UserId>{ id<UserId>(1) } : std::nullopt, id<NodeId>(n) }));
            BOOST_REQUIRE(tx->nodes().insertRoot({ id<NodeId>(n), id<SpaceId>(n), id<UserId>(1), now }));
            BOOST_REQUIRE(tx->quotas().insert(SpaceQuota{ id<SpaceId>(n) }, { { amount(100, 10), amount(0, 1), amount(0, 0) }, Revision{} }));
        }
        BOOST_REQUIRE(dynamic_cast<DriveTransaction*>(tx.get())->nodeAccounting().chargeUsed(NodeCount::from(2).value()));
        BOOST_REQUIRE(tx->quotas().insert(PublicContributionQuota{ id<SpaceId>(2), id<UserId>(1) }, { { amount(100, 10), amount(0, 1), amount(0, 0) }, Revision{} }));
        BOOST_REQUIRE(tx->settings().insertBootstrap({ id<InstanceId>(1), id<OperationId>(1), now, UserLimit::from(10).value(), amount(100, 10), amount(100, 10), Revision{} }));
        BOOST_REQUIRE(tx->identityAudit().append({ id<RequestId>(1), now, InstanceInitialized{ id<UserId>(1), id<InstanceId>(1) } }));
        BOOST_REQUIRE(tx->sessions().insert({ SessionDigest{ SessionDigest::Bytes{ 1 } }, id<UserId>(1), AuthEpoch{}, CredentialMode::Bearer, now, now + std::chrono::hours(1) }));
        BOOST_REQUIRE(tx->commit());
    }
    std::unique_ptr<MetadataTransaction> write() {
        auto result = store->beginWrite();
        BOOST_REQUIRE(result);
        return std::move(result).value();
    }
    std::unique_ptr<MetadataReadSnapshot> read() {
        auto result = store->beginRead();
        BOOST_REQUIRE(result);
        return std::move(result).value();
    }
    std::unique_ptr<SqliteConnection> raw() {
        auto result = openSqliteDatabase(backend.databasePath(), { 0 });
        BOOST_REQUIRE(result);
        return std::move(result).value();
    }
    NewDirectory directory(unsigned n, std::string name) {
        return { id<NodeId>(n), id<SpaceId>(1), id<NodeId>(1), std::move(name), id<UserId>(1), now };
    }
};
}  // namespace

BOOST_AUTO_TEST_CASE(snapshot_readers_are_not_writable_repositories_and_values_are_owned) {
    Fixture f;
    auto snapshot = f.read();
    BOOST_CHECK(dynamic_cast<UserRepository*>(&snapshot->users()) == nullptr);
    BOOST_CHECK(dynamic_cast<SessionRepository*>(&snapshot->sessions()) == nullptr);
    BOOST_CHECK(dynamic_cast<InviteRepository*>(&snapshot->invites()) == nullptr);
    BOOST_CHECK(dynamic_cast<SettingsRepository*>(&snapshot->settings()) == nullptr);
    BOOST_CHECK(dynamic_cast<SpaceRepository*>(&snapshot->spaces()) == nullptr);
    BOOST_CHECK(dynamic_cast<NodeRepository*>(&snapshot->nodes()) == nullptr);
    BOOST_CHECK(dynamic_cast<QuotaRepository*>(&snapshot->quotas()) == nullptr);
    BOOST_REQUIRE(snapshot->authorization().requireActiveSession({ SessionDigest{ SessionDigest::Bytes{ 1 } }, CredentialMode::Bearer }, now));
    BOOST_CHECK_EQUAL(snapshot->sessions().countActive(id<UserId>(1), now).value().value(), 1);
    BOOST_REQUIRE(snapshot->invites().list(PageSize::from(1).value(), {}));
    BOOST_REQUIRE(snapshot->settings().get().value());
    auto password = snapshot->users().findPasswordById(id<UserId>(1));
    auto spaces = snapshot->spaces().listAccessible(id<UserId>(1));
    auto node = snapshot->nodes().find(id<NodeId>(1));
    BOOST_REQUIRE(password && password.value() && spaces && node && node.value());
    BOOST_CHECK_EQUAL(spaces.value().size(), 2);
    BOOST_CHECK(spaces.value()[0].id == id<SpaceId>(1));
    errorIs(f.store->beginRead(), ErrorCode::InvalidArgument);
    errorIs(f.store->beginWrite(), ErrorCode::InvalidArgument);
    snapshot.reset();
    BOOST_CHECK_EQUAL(password.value()->encodedHash.view().size() > 0, true);
    BOOST_CHECK(node.value()->metadata.name.empty());
    auto tx = f.write();
    errorIs(f.store->beginRead(), ErrorCode::InvalidArgument);
    BOOST_REQUIRE(tx->commit());
}

BOOST_AUTO_TEST_CASE(user_seek_pages_include_disabled_and_admin_through_both_readers) {
    Fixture f;
    {
        auto tx = f.write();
        for (unsigned n : { 8, 3, 6 }) BOOST_REQUIRE(tx->users().insert(user(n)));
        BOOST_REQUIRE(tx->users().setEnabled(id<UserId>(3), Revision{}, false, now));
        BOOST_REQUIRE(tx->commit());
    }
    auto verify = [](UserReader& reader) {
        const auto two = PageSize::from(2).value();
        auto first = reader.list(two, {});
        BOOST_REQUIRE(first);
        BOOST_REQUIRE_EQUAL(first.value().items.size(), 2);
        BOOST_CHECK(first.value().items[0].id == id<UserId>(1));
        BOOST_CHECK(first.value().items[0].role == UserRole::Administrator);
        BOOST_CHECK(first.value().items[1].id == id<UserId>(3));
        BOOST_CHECK(!first.value().items[1].enabled);
        BOOST_REQUIRE(first.value().next);
        BOOST_CHECK(first.value().next->id == id<UserId>(3));
        auto last = reader.list(two, first.value().next);
        BOOST_REQUIRE(last);
        BOOST_REQUIRE_EQUAL(last.value().items.size(), 2);
        BOOST_CHECK(last.value().items[0].id == id<UserId>(6));
        BOOST_CHECK(last.value().items[1].id == id<UserId>(8));
        BOOST_CHECK(!last.value().next);
        auto absent = reader.list(two, UserListPosition{ id<UserId>(4) });
        BOOST_REQUIRE(absent);
        BOOST_REQUIRE_EQUAL(absent.value().items.size(), 2);
        BOOST_CHECK(absent.value().items[0].id == id<UserId>(6));
        BOOST_CHECK(!absent.value().next);
        auto end = reader.list(two, UserListPosition{ id<UserId>(8) });
        BOOST_REQUIRE(end);
        BOOST_CHECK(end.value().items.empty() && !end.value().next);
        auto all = reader.list(PageSize::from(200).value(), {});
        BOOST_REQUIRE(all);
        BOOST_CHECK_EQUAL(all.value().items.size(), 4);
        BOOST_CHECK(!all.value().next);
        std::optional<UserListPosition> after;
        std::vector<UserId> ids;
        for (unsigned pageNumber = 0; pageNumber != 5; ++pageNumber) {
            auto page = reader.list(PageSize::from(1).value(), after);
            BOOST_REQUIRE(page);
            BOOST_REQUIRE_EQUAL(page.value().items.size(), 1);
            ids.push_back(page.value().items.front().id);
            after = page.value().next;
            if (!after) break;
            BOOST_CHECK(after->id == ids.back());
        }
        const std::vector<UserId> expected{ id<UserId>(1), id<UserId>(3), id<UserId>(6), id<UserId>(8) };
        BOOST_CHECK(ids == expected);
        BOOST_CHECK(!after);
    };
    {
        auto snapshot = f.read();
        verify(snapshot->users());
    }
    auto tx = f.write();
    verify(tx->users());
    BOOST_REQUIRE(tx->commit());
    errorIs(tx->users().list(PageSize::from(1).value(), {}), ErrorCode::InvalidArgument);
    errorIs(tx->spaces().findPrivate(id<UserId>(1)), ErrorCode::InvalidArgument);
}

BOOST_AUTO_TEST_CASE(empty_user_pages_and_private_lookup_have_no_fallback) {
    Fixture f;
    f.store.reset();
    BOOST_REQUIRE(f.backend.reset({}));
    auto opened = f.backend.connect();
    BOOST_REQUIRE(opened);
    f.store = std::move(opened).value();
    auto verify = [](UserReader& users, SpaceReader& spaces) {
        auto page = users.list(PageSize::from(1).value(), {});
        BOOST_REQUIRE(page);
        BOOST_CHECK(page.value().items.empty() && !page.value().next);
        auto space = spaces.findPrivate(id<UserId>(1));
        BOOST_REQUIRE(space);
        BOOST_CHECK(!space.value());
    };
    {
        auto snapshot = f.read();
        verify(snapshot->users(), snapshot->spaces());
    }
    auto tx = f.write();
    verify(tx->users(), tx->spaces());
    BOOST_REQUIRE(tx->commit());
}

BOOST_AUTO_TEST_CASE(private_lookup_isolates_owners_and_includes_disabled_users) {
    Fixture f;
    auto tx = f.write();
    BOOST_REQUIRE(tx->users().insert(user(2)));
    BOOST_REQUIRE(tx->users().insert(user(3)));
    BOOST_REQUIRE(tx->spaces().insert({ id<SpaceId>(3), SpaceKind::Private, id<UserId>(2), id<NodeId>(3) }));
    BOOST_REQUIRE(tx->nodes().insertRoot({ id<NodeId>(3), id<SpaceId>(3), id<UserId>(2), now }));
    BOOST_REQUIRE(dynamic_cast<DriveTransaction*>(tx.get())->nodeAccounting().chargeUsed(NodeCount::from(1).value()));
    BOOST_REQUIRE(tx->users().setEnabled(id<UserId>(2), Revision{}, false, now));
    auto verify = [](SpaceReader& spaces) {
        auto admin = spaces.findPrivate(id<UserId>(1));
        auto disabled = spaces.findPrivate(id<UserId>(2));
        BOOST_REQUIRE(admin && admin.value() && disabled && disabled.value());
        BOOST_CHECK(admin.value()->id == id<SpaceId>(1));
        BOOST_CHECK(disabled.value()->id == id<SpaceId>(3));
        BOOST_CHECK(disabled.value()->ownerId == id<UserId>(2));
        BOOST_CHECK(disabled.value()->kind == SpaceKind::Private);
        BOOST_CHECK(disabled.value()->rootNodeId == id<NodeId>(3));
        for (unsigned n : { 3, 99 }) {
            auto missing = spaces.findPrivate(id<UserId>(n));
            BOOST_REQUIRE(missing);
            BOOST_CHECK(!missing.value());
        }
    };
    verify(tx->spaces());
    BOOST_REQUIRE(tx->commit());
    auto snapshot = f.read();
    verify(snapshot->spaces());
    auto owned = snapshot->spaces().findPrivate(id<UserId>(2));
    BOOST_REQUIRE(owned && owned.value());
    snapshot.reset();
    BOOST_CHECK(owned.value()->id == id<SpaceId>(3));
}

BOOST_AUTO_TEST_CASE(new_readers_share_wal_snapshot_and_return_owned_profiles) {
    Fixture f;
    auto second = f.backend.connect();
    BOOST_REQUIRE(second);
    auto snapshot = f.read();
    const auto size = PageSize::from(10).value();
    auto before = snapshot->users().list(size, {});
    BOOST_REQUIRE(before);
    BOOST_REQUIRE_EQUAL(before.value().items.size(), 1);
    auto tx = second.value()->beginWrite();
    BOOST_REQUIRE(tx);
    BOOST_REQUIRE(tx.value()->users().insert(user(2)));
    BOOST_REQUIRE(tx.value()->spaces().insert({ id<SpaceId>(3), SpaceKind::Private, id<UserId>(2), id<NodeId>(3) }));
    BOOST_REQUIRE(tx.value()->nodes().insertRoot({ id<NodeId>(3), id<SpaceId>(3), id<UserId>(2), now }));
    BOOST_REQUIRE(tx.value()->users().setDisplayName(id<UserId>(1), Revision{}, DisplayName::parse("Changed").value(), now));
    BOOST_REQUIRE(tx.value()->commit());
    auto oldPage = snapshot->users().list(size, {});
    auto oldSpace = snapshot->spaces().findPrivate(id<UserId>(2));
    BOOST_REQUIRE(oldPage && oldSpace);
    BOOST_CHECK_EQUAL(oldPage.value().items.size(), 1);
    BOOST_CHECK(!oldSpace.value());
    snapshot.reset();
    BOOST_CHECK_EQUAL(before.value().items.front().displayName.value(), "User");
    snapshot = f.read();
    auto current = snapshot->users().list(size, {});
    auto currentSpace = snapshot->spaces().findPrivate(id<UserId>(2));
    BOOST_REQUIRE(current && currentSpace && currentSpace.value());
    BOOST_REQUIRE_EQUAL(current.value().items.size(), 2);
    BOOST_CHECK_EQUAL(current.value().items.front().displayName.value(), "Changed");
    BOOST_CHECK(currentSpace.value()->id == id<SpaceId>(3));
}

BOOST_AUTO_TEST_CASE(user_page_continuation_and_private_lookup_keep_the_same_snapshot) {
    Fixture f;
    {
        auto tx = f.write();
        BOOST_REQUIRE(tx->users().insert(user(4)));
        BOOST_REQUIRE(tx->commit());
    }
    auto second = f.backend.connect();
    BOOST_REQUIRE(second);
    auto snapshot = f.read();
    // 由 SpaceReader 首先建立快照，UserReader 续页复用相同视图。
    auto missing = snapshot->spaces().findPrivate(id<UserId>(2));
    BOOST_REQUIRE(missing && !missing.value());
    auto first = snapshot->users().list(PageSize::from(1).value(), {});
    BOOST_REQUIRE(first && first.value().next);
    BOOST_CHECK(first.value().next->id == id<UserId>(1));
    auto tx = second.value()->beginWrite();
    BOOST_REQUIRE(tx);
    BOOST_REQUIRE(tx.value()->users().insert(user(2)));
    BOOST_REQUIRE(tx.value()->spaces().insert({ id<SpaceId>(3), SpaceKind::Private, id<UserId>(2), id<NodeId>(3) }));
    BOOST_REQUIRE(tx.value()->nodes().insertRoot({ id<NodeId>(3), id<SpaceId>(3), id<UserId>(2), now }));
    BOOST_REQUIRE(tx.value()->users().setEnabled(id<UserId>(4), Revision{}, false, now));
    BOOST_REQUIRE(tx.value()->commit());
    auto oldTail = snapshot->users().list(PageSize::from(1).value(), first.value().next);
    BOOST_REQUIRE(oldTail);
    BOOST_REQUIRE_EQUAL(oldTail.value().items.size(), 1);
    BOOST_CHECK(oldTail.value().items.front().id == id<UserId>(4));
    BOOST_CHECK(oldTail.value().items.front().enabled);
    BOOST_CHECK(!oldTail.value().next);
    missing = snapshot->spaces().findPrivate(id<UserId>(2));
    BOOST_REQUIRE(missing && !missing.value());
    snapshot.reset();
    snapshot = f.read();
    auto current = snapshot->users().list(PageSize::from(2).value(), first.value().next);
    BOOST_REQUIRE(current);
    BOOST_REQUIRE_EQUAL(current.value().items.size(), 2);
    BOOST_CHECK(current.value().items[0].id == id<UserId>(2));
    BOOST_CHECK(current.value().items[1].id == id<UserId>(4));
    BOOST_CHECK(!current.value().items[1].enabled && !current.value().next);
    auto personal = snapshot->spaces().findPrivate(id<UserId>(2));
    BOOST_REQUIRE(personal && personal.value());
    BOOST_CHECK(personal.value()->id == id<SpaceId>(3));
}

BOOST_AUTO_TEST_CASE(transaction_readers_see_own_writes_and_observers_see_only_commits) {
    for (bool commit : { false, true }) {
        Fixture f;
        auto observer = f.backend.connect();
        BOOST_REQUIRE(observer);
        auto tx = f.write();
        BOOST_REQUIRE(tx->users().insert(user(2)));
        BOOST_REQUIRE(tx->spaces().insert({ id<SpaceId>(3), SpaceKind::Private, id<UserId>(2), id<NodeId>(3) }));
        BOOST_REQUIRE(tx->nodes().insertRoot({ id<NodeId>(3), id<SpaceId>(3), id<UserId>(2), now }));
        BOOST_REQUIRE(dynamic_cast<DriveTransaction*>(tx.get())->nodeAccounting().chargeUsed(NodeCount::from(1).value()));
        auto ownPage = tx->users().list(PageSize::from(2).value(), {});
        auto ownSpace = tx->spaces().findPrivate(id<UserId>(2));
        BOOST_REQUIRE(ownPage && ownSpace && ownSpace.value());
        BOOST_REQUIRE_EQUAL(ownPage.value().items.size(), 2);
        BOOST_CHECK(ownPage.value().items[1].id == id<UserId>(2));
        BOOST_CHECK(ownSpace.value()->ownerId == id<UserId>(2));
        errorIs(observer.value()->beginWrite(), ErrorCode::Retryable);
        auto before = observer.value()->beginRead();
        BOOST_REQUIRE(before);
        auto oldPage = before.value()->users().list(PageSize::from(2).value(), {});
        auto oldSpace = before.value()->spaces().findPrivate(id<UserId>(2));
        BOOST_REQUIRE(oldPage && oldSpace);
        BOOST_CHECK_EQUAL(oldPage.value().items.size(), 1);
        BOOST_CHECK(!oldSpace.value());
        if (commit) BOOST_REQUIRE(tx->commit());
        tx.reset();
        // 已建立的观察快照在提交后也不切换视图。
        auto pinned = before.value()->users().list(PageSize::from(2).value(), {});
        BOOST_REQUIRE(pinned);
        BOOST_CHECK_EQUAL(pinned.value().items.size(), 1);
        before.value().reset();
        auto after = observer.value()->beginRead();
        BOOST_REQUIRE(after);
        auto page = after.value()->users().list(PageSize::from(2).value(), {});
        auto space = after.value()->spaces().findPrivate(id<UserId>(2));
        BOOST_REQUIRE(page && space);
        BOOST_CHECK_EQUAL(page.value().items.size(), commit ? 2 : 1);
        BOOST_CHECK(space.value().has_value() == commit);
        // 事务返回自有值；其生命周期与提交或回滚无关。
        BOOST_CHECK_EQUAL(ownPage.value().items[1].displayName.value(), "User");
        BOOST_CHECK(ownSpace.value()->id == id<SpaceId>(3));
    }
}

BOOST_AUTO_TEST_CASE(user_listing_does_not_read_password_columns) {
    Fixture f;
    auto raw = f.raw();
    auto denyPassword = [](void*, int operation, const char* table, const char* column, const char*, const char*) {
        if (operation == SQLITE_READ && table && column && std::string_view(table) == "users" &&
            (std::string_view(column) == "password_hash" || std::string_view(column) == "password_version")) return SQLITE_DENY;
        return SQLITE_OK;
    };
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(raw->handle(), denyPassword, nullptr), SQLITE_OK);
    bool active = false;
    auto snapshot = makeSqliteReadSnapshot(*raw, active, budget);
    BOOST_REQUIRE(snapshot);
    auto page = snapshot.value()->users().list(PageSize::from(1).value(), {});
    BOOST_REQUIRE(page);
    BOOST_REQUIRE_EQUAL(page.value().items.size(), 1);
    BOOST_CHECK(page.value().items.front().id == id<UserId>(1));
    BOOST_CHECK(!page.value().next);
}

BOOST_AUTO_TEST_CASE(wal_snapshot_preserves_related_reads_while_second_connection_commits) {
    Fixture f;
    auto second = f.backend.connect();
    BOOST_REQUIRE(second);
    auto snapshot = f.read();
    BOOST_CHECK_EQUAL(snapshot->users().find(id<UserId>(1)).value()->revision.value(), 1);
    auto tx = second.value()->beginWrite();
    BOOST_REQUIRE(tx);
    BOOST_REQUIRE(tx.value()->users().setDisplayName(id<UserId>(1), Revision{}, DisplayName::parse("Changed").value(), now));
    BOOST_REQUIRE(tx.value()->quotas().chargeUsed(SpaceQuota{ id<SpaceId>(1) }, amount(0, 1)));
    BOOST_REQUIRE(tx.value()->nodes().insertDirectory(f.directory(3, "entry")));
    BOOST_REQUIRE(tx.value()->commit());
    BOOST_CHECK_EQUAL(snapshot->users().find(id<UserId>(1)).value()->revision.value(), 1);
    BOOST_CHECK_EQUAL(snapshot->quotas().get(SpaceQuota{ id<SpaceId>(1) }).value().state.used.nodes.value(), 1);
    BOOST_CHECK(!snapshot->nodes().find(id<NodeId>(3)).value());
    snapshot.reset();
    snapshot = f.read();
    BOOST_CHECK_EQUAL(snapshot->users().find(id<UserId>(1)).value()->revision.value(), 2);
    BOOST_CHECK_EQUAL(snapshot->quotas().get(SpaceQuota{ id<SpaceId>(1) }).value().state.used.nodes.value(), 2);
    BOOST_REQUIRE(snapshot->nodes().find(id<NodeId>(3)).value());
}

BOOST_AUTO_TEST_CASE(read_failure_ends_scope_and_sqlite_query_only_cannot_upgrade) {
    Fixture f;
    {
        auto snapshot = f.read();
        errorIs(snapshot->spaces().listAccessible(id<UserId>(99)), ErrorCode::IntegrityError);
    }
    auto raw = f.raw();
    bool active = false;
    {
        SqliteTransactionContext context(*raw, active);
        BOOST_REQUIRE(context.beginRead());
        errorIs(raw->execute("UPDATE users SET display_name='blocked'"), ErrorCode::InternalError);
        BOOST_CHECK_EQUAL(raw->diagnostic().extendedCode & 0xff, SQLITE_READONLY);
    }
    BOOST_CHECK(!active);
    BOOST_REQUIRE(raw->beginWrite());
    BOOST_REQUIRE(raw->execute("UPDATE users SET display_name='allowed'"));
    BOOST_REQUIRE(raw->rollback());
    auto tx = f.write();
    BOOST_REQUIRE(tx->commit());
}

BOOST_AUTO_TEST_CASE(read_begin_and_cleanup_failures_leave_no_usable_leaked_scope) {
    Fixture f;
    auto raw = f.raw();
    bool active = false;
    auto denyBegin = [](void*, int operation, const char* first, const char*, const char*, const char*) {
        return operation == SQLITE_TRANSACTION && first && std::string_view(first) == "BEGIN" ? SQLITE_DENY : SQLITE_OK;
    };
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(raw->handle(), denyBegin, nullptr), SQLITE_OK);
    {
        SqliteTransactionContext context(*raw, active);
        errorIs(context.beginRead(), ErrorCode::InternalError);
    }
    BOOST_CHECK(!active && !raw->inTransaction());
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(raw->handle(), nullptr, nullptr), SQLITE_OK);
    BOOST_REQUIRE(raw->beginWrite());
    BOOST_REQUIRE(raw->execute("UPDATE users SET display_name='probe'"));
    BOOST_REQUIRE(raw->rollback());
    {
        SqliteTransactionContext context(*raw, active);
        BOOST_REQUIRE(context.beginRead());
        auto denyRollback = [](void*, int operation, const char* first, const char*, const char*, const char*) {
            return operation == SQLITE_TRANSACTION && first && std::string_view(first) == "ROLLBACK" ? SQLITE_DENY : SQLITE_OK;
        };
        BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(raw->handle(), denyRollback, nullptr), SQLITE_OK);
    }
    BOOST_CHECK(!active && !raw->usable());
    errorIs(raw->beginWrite(), ErrorCode::InternalError);
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(raw->handle(), nullptr, nullptr), SQLITE_OK);
}

BOOST_AUTO_TEST_CASE(root_creation_and_quota_seed_are_atomic_and_bill_once) {
    Fixture f;
    auto tx = f.write();
    BOOST_REQUIRE(tx->users().insert(user(2)));
    BOOST_REQUIRE(tx->spaces().insert({ id<SpaceId>(3), SpaceKind::Private, id<UserId>(2), id<NodeId>(3) }));
    BOOST_REQUIRE(tx->nodes().insertRoot({ id<NodeId>(3), id<SpaceId>(3), id<UserId>(2), now }));
    BOOST_REQUIRE(dynamic_cast<DriveTransaction*>(tx.get())->nodeAccounting().chargeUsed(NodeCount::from(1).value()));
    BOOST_REQUIRE(tx->quotas().insert(SpaceQuota{ id<SpaceId>(3) }, { { amount(100, 10), amount(0, 1), amount(0, 0) }, Revision{} }));
    BOOST_REQUIRE(tx->quotas().insert(PublicContributionQuota{ id<SpaceId>(2), id<UserId>(2) }, { { amount(100, 10), amount(0, 0), amount(0, 0) }, Revision{} }));
    BOOST_REQUIRE(tx->commit());
    auto opened = openSqliteMetadataStore(f.backend.databasePath(), { budget, { 0 } });
    BOOST_REQUIRE(opened);
    auto read = f.read();
    auto spaces = read->spaces().listAccessible(id<UserId>(2));
    BOOST_REQUIRE(spaces);
    BOOST_CHECK_EQUAL(spaces.value().size(), 2);
    auto quota = read->quotas().get(SpaceQuota{ id<SpaceId>(3) });
    BOOST_REQUIRE(quota);
    BOOST_CHECK_EQUAL(quota.value().state.used.nodes.value(), 1);
    BOOST_CHECK_EQUAL(quota.value().revision.value(), 1);
    read.reset();
    tx = f.write();
    errorIs(tx->nodes().insertRoot({ id<NodeId>(3), id<SpaceId>(3), id<UserId>(2), now }), ErrorCode::IntegrityError);
    BOOST_CHECK(!tx->commit());
    tx = f.write();
    BOOST_REQUIRE(tx->users().insert(user(3)));
    BOOST_REQUIRE(tx->spaces().insert({ id<SpaceId>(4), SpaceKind::Private, id<UserId>(3), id<NodeId>(4) }));
    errorIs(tx->commit(), ErrorCode::IntegrityError);
    BOOST_CHECK(!f.read()->users().find(id<UserId>(3)).value());
}

BOOST_AUTO_TEST_CASE(node_seek_pagination_has_binary_order_and_file_metadata) {
    Fixture f;
    {
        auto tx = f.write();
        for (const auto& item : { std::pair<unsigned, const char*>{ 3, "z" }, { 4, "A" }, { 5, "a" }, { 6, "中文" } }) {
            BOOST_REQUIRE(tx->nodes().insertDirectory(f.directory(item.first, item.second)));
        }
        BOOST_REQUIRE(tx->commit());
    }
    auto raw = f.raw();
    BOOST_REQUIRE(raw->execute(
        "INSERT INTO blobs(id,size,sha256,state,created_at,updated_at,revision,charged_bytes) VALUES('00000000-0000-4000-8000-000000000009',1099511627776,zeroblob(32),0,0,0,1,1099511627776); "
        "INSERT INTO nodes(id,space_id,parent_id,name,kind,created_by,blob_id,revision,trashed,created_at,updated_at) VALUES('00000000-0000-4000-8000-000000000009','00000000-0000-4000-8000-000000000001','00000000-0000-4000-8000-000000000001','file',1,'00000000-0000-4000-8000-000000000001','00000000-0000-4000-8000-000000000009',1,0,123,123)"));
    auto snapshot = f.read();
    std::vector<std::string> names;
    std::optional<ChildPosition> after;
    do {
        auto page = snapshot->nodes().listChildren(id<SpaceId>(1), id<NodeId>(1), PageSize::from(2).value(), after);
        BOOST_REQUIRE(page);
        BOOST_CHECK_LE(page.value().items.size(), 2);
        for (const auto& item : page.value().items) names.push_back(item.metadata.name);
        after = page.value().next;
    } while (after);
    const std::vector<std::string> expected{ "A", "a", "file", "z", "中文" };
    BOOST_CHECK(names == expected);
    auto file = snapshot->nodes().find(id<NodeId>(9));
    BOOST_REQUIRE(file && file.value());
    const auto* content = std::get_if<FileContentMetadata>(&file.value()->content);
    BOOST_REQUIRE(content);
    BOOST_CHECK_EQUAL(content->size.value(), 1099511627776LL);
    BOOST_CHECK(std::holds_alternative<DirectoryContent>(snapshot->nodes().find(id<NodeId>(4)).value()->content));
    auto empty = snapshot->nodes().listChildren(id<SpaceId>(1), id<NodeId>(4), PageSize::from(200).value(), {});
    BOOST_REQUIRE(empty);
    BOOST_CHECK(empty.value().items.empty() && !empty.value().next);
}

BOOST_AUTO_TEST_CASE(hidden_ancestors_and_bad_parent_never_return_children) {
    Fixture f;
    {
        auto tx = f.write();
        BOOST_REQUIRE(tx->nodes().insertDirectory(f.directory(3, "hidden")));
        BOOST_REQUIRE(tx->nodes().insertDirectory({ id<NodeId>(4), id<SpaceId>(1), id<NodeId>(3), "child", id<UserId>(1), now }));
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto tx = f.write();
        auto* drive = dynamic_cast<DriveTransaction*>(tx.get());
        BOOST_REQUIRE(drive);
        auto tree = drive->tree().activeSubtree(id<NodeId>(3), {}).value();
        auto summary = summarizeTree(tree).value();
        TrashEntry e{ id<TrashId>(1), id<SpaceId>(1), id<NodeId>(3), id<NodeId>(1), "hidden", NodeKind::Directory, id<UserId>(1), now, trashExpiry(now).value(), {}, summary.total.nodes, summary.total.bytes, id<UserId>(1) };
        BOOST_REQUIRE(drive->trash().insert(e));
        BOOST_REQUIRE(drive->tree().detach(tree, { id<NodeId>(3), {} }, e));
        BOOST_REQUIRE(tx->commit());
    }
    {
        auto snapshot = f.read();
        BOOST_CHECK(!snapshot->nodes().find(id<NodeId>(4)).value());
        errorIs(snapshot->nodes().listChildren(id<SpaceId>(1), id<NodeId>(3), PageSize::from(1).value(), {}), ErrorCode::NotFound);
    }
    {
        auto snapshot = f.read();
        errorIs(snapshot->nodes().listChildren(id<SpaceId>(2), id<NodeId>(1), PageSize::from(1).value(), {}), ErrorCode::NotFound);
    }
    {
        auto snapshot = f.read();
        errorIs(snapshot->nodes().listChildren(id<SpaceId>(1), id<NodeId>(1), PageSize::from(1).value(), ChildPosition{ "..", id<NodeId>(5) }), ErrorCode::InvalidArgument);
    }
}

BOOST_AUTO_TEST_CASE(quota_revision_limit_noop_and_reservation_floor) {
    Fixture f;
    BOOST_REQUIRE(f.raw()->execute("UPDATE space_quotas SET reserved_nodes=2 WHERE space_id='00000000-0000-4000-8000-000000000001'"));
    const QuotaScope scope = SpaceQuota{ id<SpaceId>(1) };
    auto tx = f.write();
    auto unchanged = tx->quotas().setLimit(scope, Revision{}, amount(100, 10));
    BOOST_REQUIRE(unchanged);
    BOOST_CHECK_EQUAL(unchanged.value().revision.value(), 1);
    BOOST_REQUIRE(tx->quotas().chargeUsed(scope, amount(0, 0)));
    auto changed = tx->quotas().setLimit(scope, Revision{}, amount(50, 3));
    BOOST_REQUIRE(changed);
    BOOST_CHECK_EQUAL(changed.value().revision.value(), 2);
    BOOST_REQUIRE(tx->identityAudit().append({ id<RequestId>(2), now, QuotaChanged{ id<UserId>(1), scope } }));
    BOOST_REQUIRE(tx->commit());
    tx = f.write();
    errorIs(tx->quotas().setLimit(scope, Revision{}, amount(50, 3)), ErrorCode::PreconditionFailed);
    BOOST_CHECK(!tx->commit());
    tx = f.write();
    errorIs(tx->quotas().setLimit(scope, Revision::from(2).value(), amount(50, 2)), ErrorCode::QuotaExceeded);
    BOOST_CHECK(!tx->commit());
    BOOST_CHECK_EQUAL(f.read()->quotas().get(scope).value().revision.value(), 2);
}

BOOST_AUTO_TEST_CASE(caller_appends_quota_audit_only_for_actual_limit_changes) {
    Fixture f;
    const QuotaScope scopes[] = { SpaceQuota{ id<SpaceId>(1) }, SpaceQuota{ id<SpaceId>(2) },
                                  PublicContributionQuota{ id<SpaceId>(2), id<UserId>(1) } };
    for (const auto& scope : scopes) {
        for (unsigned attempt = 0; attempt != 2; ++attempt) {
            auto tx = f.write();
            auto before = tx->quotas().get(scope);
            BOOST_REQUIRE(before);
            auto changed = tx->quotas().setLimit(scope, before.value().revision, amount(200, 20));
            BOOST_REQUIRE(changed);
            if (changed.value().revision != before.value().revision)
                BOOST_REQUIRE(tx->identityAudit().append({ id<RequestId>(5), now, QuotaChanged{ id<UserId>(1), scope } }));
            BOOST_REQUIRE(tx->commit());
        }
    }
    auto raw = f.raw();
    auto query = SqliteStatement::prepare(*raw, "SELECT count(*) FROM identity_audit_events WHERE action='quota_changed'");
    BOOST_REQUIRE(query);
    auto row = query.value()->step();
    BOOST_REQUIRE(row && row.value());
    BOOST_CHECK_EQUAL(query.value()->integer(0).value(), 3);
    auto snapshot = f.read();
    for (const auto& scope : scopes) {
        auto quota = snapshot->quotas().get(scope);
        BOOST_REQUIRE(quota);
        BOOST_CHECK_EQUAL(quota.value().revision.value(), 2);
    }
}

BOOST_AUTO_TEST_CASE(quota_revision_overflow_and_audit_failure_roll_back_all_writes) {
    Fixture f;
    const QuotaScope space = SpaceQuota{ id<SpaceId>(1) };
    const QuotaScope contribution = PublicContributionQuota{ id<SpaceId>(2), id<UserId>(1) };
    BOOST_REQUIRE(f.raw()->execute("UPDATE public_contribution_quotas SET revision=9223372036854775807"));
    auto tx = f.write();
    BOOST_REQUIRE(tx->quotas().chargeUsed(space, amount(0, 1)));
    errorIs(tx->quotas().chargeUsed(contribution, amount(0, 1)), ErrorCode::IntegrityError);
    BOOST_CHECK(!tx->commit());
    BOOST_CHECK_EQUAL(f.read()->quotas().get(space).value().state.used.nodes.value(), 1);
    BOOST_REQUIRE(f.raw()->execute("CREATE TRIGGER fail_identity_audit BEFORE INSERT ON identity_audit_events BEGIN SELECT RAISE(ABORT,'injected'); END"));
    tx = f.write();
    BOOST_REQUIRE(tx->quotas().setLimit(space, Revision{}, amount(100, 20)));
    errorIs(tx->identityAudit().append({ id<RequestId>(3), now, QuotaChanged{ id<UserId>(1), space } }), ErrorCode::IntegrityError);
    BOOST_CHECK(!tx->commit());
    BOOST_CHECK_EQUAL(f.read()->quotas().get(space).value().revision.value(), 1);
}

BOOST_AUTO_TEST_CASE(production_open_requires_complete_bootstrap_and_explicit_hash_budget) {
    Fixture f;
    SqliteMetadataOptions unconfigured;
    errorIs(openSqliteMetadataStore(f.backend.databasePath(), unconfigured), ErrorCode::InvalidArgument);
    BOOST_REQUIRE(openSqliteMetadataStore(f.backend.databasePath(), { budget, { 0 } }));
    errorIs(openSqliteMetadataStore(f.backend.databasePath(), { {}, { 0 } }), ErrorCode::InvalidArgument);
    errorIs(openSqliteMetadataStore(f.backend.databasePath(), { { 4096, 1, 1 }, { 0 } }), ErrorCode::IntegrityError);
    BOOST_REQUIRE(f.raw()->execute("DELETE FROM public_contribution_quotas"));
    errorIs(openSqliteMetadataStore(f.backend.databasePath(), { budget, { 0 } }), ErrorCode::IntegrityError);
    f.store.reset();
    BOOST_REQUIRE(f.backend.reset({}));
    errorIs(openSqliteMetadataStore(f.backend.databasePath(), { budget, { 0 } }), ErrorCode::IntegrityError);
}
BOOST_AUTO_TEST_CASE(new_repository_accessors_close_with_transaction_and_root_owner_is_checked) {
    Fixture f;
    auto old = f.write();
    BOOST_REQUIRE(old->commit());
    auto next = f.write();
    errorIs(old->users().countUsers(), ErrorCode::InvalidArgument);
    errorIs(old->sessions().countActive(id<UserId>(1), now), ErrorCode::InvalidArgument);
    errorIs(old->invites().find(id<InviteId>(1)), ErrorCode::InvalidArgument);
    errorIs(old->settings().get(), ErrorCode::InvalidArgument);
    errorIs(old->quotas().get(SpaceQuota{ id<SpaceId>(1) }), ErrorCode::InvalidArgument);
    errorIs(old->nodes().find(id<NodeId>(1)), ErrorCode::InvalidArgument);
    errorIs(old->identityAudit().append({ id<RequestId>(4), now, DisplayNameChanged{ id<UserId>(1) } }), ErrorCode::InvalidArgument);
    old.reset();
    BOOST_REQUIRE(next->users().insert(user(2)));
    BOOST_REQUIRE(next->spaces().insert({ id<SpaceId>(3), SpaceKind::Private, id<UserId>(2), id<NodeId>(3) }));
    errorIs(next->nodes().insertRoot({ id<NodeId>(3), id<SpaceId>(3), id<UserId>(1), now }), ErrorCode::IntegrityError);
    BOOST_CHECK(!next->commit());
    BOOST_CHECK(!f.read()->users().find(id<UserId>(2)).value());
}

BOOST_AUTO_TEST_CASE(quota_compare_after_independent_writer_and_set_limit_overflow) {
    Fixture f;
    auto second = f.backend.connect();
    BOOST_REQUIRE(second);
    const QuotaScope scope = PublicContributionQuota{ id<SpaceId>(2), id<UserId>(1) };
    auto first = f.write();
    errorIs(second.value()->beginWrite(), ErrorCode::Retryable);
    BOOST_REQUIRE(first->quotas().setLimit(scope, Revision{}, amount(200, 20)));
    BOOST_REQUIRE(first->commit());
    auto after = second.value()->beginWrite();
    BOOST_REQUIRE(after);
    errorIs(after.value()->quotas().setLimit(scope, Revision{}, amount(300, 30)), ErrorCode::PreconditionFailed);
    BOOST_CHECK(!after.value()->commit());
    BOOST_REQUIRE(f.raw()->execute("UPDATE public_contribution_quotas SET revision=9223372036854775807"));
    first = f.write();
    const auto maximum = Revision::from(std::numeric_limits<std::int64_t>::max()).value();
    BOOST_REQUIRE(first->quotas().setLimit(scope, maximum, amount(200, 20)));
    errorIs(first->quotas().setLimit(scope, maximum, amount(300, 30)), ErrorCode::IntegrityError);
    BOOST_CHECK(!first->commit());
    BOOST_CHECK_EQUAL(f.read()->quotas().get(scope).value().state.limit.nodes.value(), 20);
}

BOOST_AUTO_TEST_CASE(production_open_rejects_missing_required_rows_and_unbilled_roots) {
    for (const char* damage : {
             "DELETE FROM identity_audit_events; DELETE FROM instance_settings",
             "DELETE FROM space_quotas",
             "UPDATE space_quotas SET used_nodes=0",
             "UPDATE public_contribution_quotas SET used_nodes=0" }) {
        Fixture f;
        BOOST_REQUIRE(f.raw()->execute(damage));
        errorIs(openSqliteMetadataStore(f.backend.databasePath(), { budget, { 0 } }), ErrorCode::IntegrityError);
    }
    Fixture f;
    auto tx = f.write();
    BOOST_REQUIRE(tx->users().insert(user(2)));
    BOOST_REQUIRE(tx->commit());
    errorIs(openSqliteMetadataStore(f.backend.databasePath(), { budget, { 0 } }), ErrorCode::IntegrityError);
}

BOOST_AUTO_TEST_CASE(file_blob_corruption_fails_closed_in_find_and_page) {
    Fixture f;
    auto raw = f.raw();
    BOOST_REQUIRE(raw->execute(
        "PRAGMA foreign_keys=OFF; DROP TRIGGER nodes_blob_insert; "
        "INSERT INTO nodes(id,space_id,parent_id,name,kind,created_by,blob_id,revision,trashed,created_at,updated_at) VALUES('00000000-0000-4000-8000-000000000009','00000000-0000-4000-8000-000000000001','00000000-0000-4000-8000-000000000001','broken',1,'00000000-0000-4000-8000-000000000001','00000000-0000-4000-8000-000000000009',1,0,123,123)"));
    {
        auto snapshot = f.read();
        errorIs(snapshot->nodes().find(id<NodeId>(9)), ErrorCode::IntegrityError);
    }
    {
        auto snapshot = f.read();
        errorIs(snapshot->nodes().listChildren(id<SpaceId>(1), id<NodeId>(1), PageSize::from(200).value(), {}), ErrorCode::IntegrityError);
    }
}

}  // namespace anof::infrastructure::sqlite::testing
