#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestStore.h>
#define BOOST_TEST_MODULE AnofSqliteMetadataTransaction
#include <boost/test/unit_test.hpp>

#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace anof::infrastructure::sqlite {
namespace {

constexpr const char* kUser = "00000000-0000-4000-8000-000000000001";
constexpr const char* kMember = "00000000-0000-4000-8000-000000000002";
constexpr const char* kPrivate = "00000000-0000-4000-8000-000000000011";
constexpr const char* kPublic = "00000000-0000-4000-8000-000000000012";
constexpr const char* kPrivateRoot = "00000000-0000-4000-8000-000000000021";
constexpr const char* kPublicRoot = "00000000-0000-4000-8000-000000000022";
constexpr const char* kMissing = "00000000-0000-4000-8000-000000000099";
const UtcTime kNow{ std::chrono::microseconds{ 1000000 } };
const UtcTime kExpires{ std::chrono::microseconds{ 2000000 } };
const SessionDigest kDigest{ SessionDigest::Bytes{ 1 } };

std::string quoted(const char* value) { return "'" + std::string(value) + "'"; }

template <typename T>
void errorIs(const Result<T>& result, ErrorCode expected) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == expected);
}

std::unique_ptr<SqliteStatement> prepare(SqliteConnection& connection, std::string_view sql) {
    auto result = SqliteStatement::prepare(connection, sql);
    BOOST_REQUIRE(result);
    return std::move(result).value();
}

std::int64_t scalar(SqliteConnection& connection, std::string_view sql) {
    auto statement = prepare(connection, sql);
    auto row = statement->step();
    BOOST_REQUIRE(row && row.value());
    auto value = statement->integer(0);
    BOOST_REQUIRE(value);
    return value.value();
}

std::unique_ptr<MetadataTransaction> begin(MetadataStore& store) {
    auto result = store.beginWrite();
    BOOST_REQUIRE(result);
    return std::move(result).value();
}

class Database {
public:
    Database() {
        char pattern[] = "/tmp/anof-sqlite-transaction-XXXXXX";
        const char* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("Cannot create test directory");
        directory_ = created;
    }
    ~Database() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    std::filesystem::path path() const { return directory_ / "metadata.sqlite3"; }
    std::unique_ptr<SqliteConnection> raw() {
        auto result = openSqliteDatabase(path(), { 0 });
        BOOST_REQUIRE(result);
        return std::move(result).value();
    }
    std::unique_ptr<MetadataStore> store() {
        auto result = testing::openTestMetadataStore(path(), { 0 });
        BOOST_REQUIRE(result);
        return std::move(result).value();
    }
    void seed() {
        BOOST_REQUIRE(createSqliteDatabase(path(), { 0 }));
        auto connection = raw();
        BOOST_REQUIRE(connection->beginWrite());
        BOOST_REQUIRE(connection->execute("INSERT INTO users VALUES (" + quoted(kUser) + ", 1, 1, 0, 'admin', 'Administrator', '" + std::string("$argon2id$v=19$m=8192,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") + "', 1, 1, 0, 0), (" + quoted(kMember) + ", 0, 1, 0, 'member', 'Member', '" + std::string("$argon2id$v=19$m=8192,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") + "', 1, 1, 0, 0)"));
        addSpace(*connection, kPrivate, kPrivateRoot, false);
        addSpace(*connection, kPublic, kPublicRoot, true);
        auto statement = prepare(*connection, "INSERT INTO sessions VALUES (?1, ?2, 0, ?3, 0, -9223372036854775808, 'bearer')");
        BOOST_REQUIRE(statement->bindBlob(1, kDigest.bytes().data(), kDigest.bytes().size()));
        BOOST_REQUIRE(statement->bindText(2, kUser));
        BOOST_REQUIRE(statement->bindInt64(3, kExpires.time_since_epoch().count()));
        auto row = statement->step();
        BOOST_REQUIRE(row && !row.value());
        statement.reset();
        BOOST_REQUIRE(connection->commit());
    }
private:
    static void addSpace(SqliteConnection& connection, const char* space, const char* root, bool isPublic) {
        BOOST_REQUIRE(connection.execute("INSERT INTO spaces VALUES (" + quoted(space) + ", " +
            (isPublic ? "1, NULL, " : "0, " + quoted(kUser) + ", ") + quoted(root) + ")"));
        BOOST_REQUIRE(connection.execute("INSERT INTO nodes(id,space_id,parent_id,name,kind,created_by,blob_id,revision,trashed,created_at,updated_at) VALUES (" + quoted(root) + ", " + quoted(space) +
                                         ", NULL, '', 0, " + quoted(kUser) + ", NULL, 1, 0, 0, 0)"));
    }
    std::filesystem::path directory_;
};

void expectIdentity(MetadataTransaction& transaction, const char* user, UserRole role, std::int64_t epoch = 0) {
    auto identity = transaction.authorization().requireActiveSession({ kDigest, CredentialMode::Bearer }, kNow);
    BOOST_REQUIRE(identity);
    BOOST_CHECK(identity.value().userId == UserId::parse(user).value());
    BOOST_CHECK(identity.value().role == role);
    BOOST_TEST(identity.value().authEpoch.value() == epoch);
}

}  // namespace

BOOST_AUTO_TEST_CASE(store_requires_existing_database_and_exposes_typed_ports) {
    Database database;
    errorIs(testing::openTestMetadataStore(database.path()), ErrorCode::InternalError);
    BOOST_CHECK(!std::filesystem::exists(database.path()));
    database.seed();
    auto store = database.store();
    auto transaction = begin(*store);
    expectIdentity(*transaction, kUser, UserRole::Administrator);
    const auto privateId = SpaceId::parse(kPrivate).value();
    auto privateSpace = transaction->spaces().find(privateId);
    BOOST_REQUIRE(privateSpace && privateSpace.value());
    BOOST_CHECK(privateSpace.value()->id == privateId);
    BOOST_CHECK(privateSpace.value()->kind == SpaceKind::Private);
    BOOST_REQUIRE(privateSpace.value()->ownerId);
    BOOST_CHECK(*privateSpace.value()->ownerId == UserId::parse(kUser).value());
    BOOST_CHECK(privateSpace.value()->rootNodeId == NodeId::parse(kPrivateRoot).value());
    auto publicSpace = transaction->spaces().find(SpaceId::parse(kPublic).value());
    BOOST_REQUIRE(publicSpace && publicSpace.value());
    BOOST_CHECK(publicSpace.value()->kind == SpaceKind::Public);
    BOOST_CHECK(!publicSpace.value()->ownerId);
    BOOST_CHECK(publicSpace.value()->rootNodeId == NodeId::parse(kPublicRoot).value());
    auto missing = transaction->spaces().find(SpaceId::parse(kMissing).value());
    BOOST_REQUIRE(missing);
    BOOST_CHECK(!missing.value());
    BOOST_REQUIRE(transaction->commit());
}

BOOST_AUTO_TEST_CASE(session_failures_are_uniform_and_failed_transactions_cannot_commit) {
    const std::string changes[] = {
        "DELETE FROM sessions",
        "UPDATE sessions SET expires_at=1000000",
        "UPDATE sessions SET expires_at=999999",
        "UPDATE sessions SET revoked=1",
        "UPDATE users SET enabled=0 WHERE id=" + quoted(kMember),
        "UPDATE users SET auth_epoch=1 WHERE id=" + quoted(kMember),
        "UPDATE sessions SET auth_epoch=1"
    };
    for (const auto& change : changes) {
        Database database;
        database.seed();
        { auto connection = database.raw(); BOOST_REQUIRE(connection->execute("UPDATE sessions SET user_id=" + quoted(kMember))); BOOST_REQUIRE(connection->execute(change)); }
        auto store = database.store();
        auto transaction = begin(*store);
        errorIs(transaction->authorization().requireActiveSession({ kDigest, CredentialMode::Bearer }, kNow), ErrorCode::Unauthenticated);
        errorIs(transaction->commit(), ErrorCode::InternalError);
        errorIs(transaction->spaces().find(SpaceId::parse(kPublic).value()), ErrorCode::InvalidArgument);
        auto next = begin(*store);
        BOOST_REQUIRE(next->commit());
    }
}

BOOST_AUTO_TEST_CASE(session_digest_is_exact_and_expiry_is_rechecked_with_supplied_time) {
    Database database;
    database.seed();
    auto store = database.store();
    {
        auto transaction = begin(*store);
        auto bytes = kDigest.bytes();
        bytes.back() = 1;
        errorIs(transaction->authorization().requireActiveSession({ SessionDigest(bytes), CredentialMode::Bearer }, kNow), ErrorCode::Unauthenticated);
    }
    auto transaction = begin(*store);
    auto almostExpired = transaction->authorization().requireActiveSession({ kDigest, CredentialMode::Bearer }, kExpires - std::chrono::microseconds(1));
    BOOST_REQUIRE(almostExpired);
    errorIs(transaction->authorization().requireActiveSession({ kDigest, CredentialMode::Bearer }, kExpires), ErrorCode::Unauthenticated);
    errorIs(transaction->commit(), ErrorCode::InternalError);
}

BOOST_AUTO_TEST_CASE(member_and_int64_epoch_round_trip_without_identity_cache) {
    Database database;
    database.seed();
    auto store = database.store();
    {
        auto transaction = begin(*store);
        expectIdentity(*transaction, kUser, UserRole::Administrator);
    }
    {
        auto connection = database.raw();
        BOOST_REQUIRE(connection->beginWrite());
        BOOST_REQUIRE(connection->execute("UPDATE users SET auth_epoch=9223372036854775807 WHERE id=" + quoted(kMember)));
        BOOST_REQUIRE(connection->execute("UPDATE sessions SET user_id=" + quoted(kMember) + ", auth_epoch=9223372036854775807"));
        BOOST_REQUIRE(connection->commit());
    }
    auto transaction = begin(*store);
    expectIdentity(*transaction, kMember, UserRole::Member, std::numeric_limits<std::int64_t>::max());
    // SpaceRepository 返回存储值；成员是否能访问该私有空间由应用领域策略决定。
    auto space = transaction->spaces().find(SpaceId::parse(kPrivate).value());
    BOOST_REQUIRE(space && space.value());
    BOOST_CHECK(space.value()->ownerId.value() == UserId::parse(kUser).value());
    BOOST_REQUIRE(transaction->commit());
}

BOOST_AUTO_TEST_CASE(old_transaction_cannot_read_commit_or_rollback_new_transaction) {
    Database database;
    database.seed();
    auto store = database.store();
    auto old = begin(*store);
    auto& oldAuthorization = old->authorization();
    auto& oldSpaces = old->spaces();
    errorIs(store->beginWrite(), ErrorCode::InvalidArgument);
    BOOST_REQUIRE(old->commit());
    auto current = begin(*store);
    errorIs(oldAuthorization.requireActiveSession({ kDigest, CredentialMode::Bearer }, kNow), ErrorCode::InvalidArgument);
    errorIs(oldSpaces.find(SpaceId::parse(kPublic).value()), ErrorCode::InvalidArgument);
    errorIs(old->commit(), ErrorCode::InvalidArgument);
    old.reset();
    expectIdentity(*current, kUser, UserRole::Administrator);
    auto competitor = database.store();
    errorIs(competitor->beginWrite(), ErrorCode::Retryable);
    BOOST_REQUIRE(current->commit());
    auto next = begin(*competitor);
    BOOST_REQUIRE(next->commit());
}

BOOST_AUTO_TEST_CASE(independent_connection_cannot_revoke_between_authentication_and_commit) {
    Database database;
    database.seed();
    auto writer = database.raw();
    auto store = database.store();
    auto transaction = begin(*store);
    expectIdentity(*transaction, kUser, UserRole::Administrator);
    errorIs(writer->beginWrite(), ErrorCode::Retryable);
    errorIs(writer->execute("UPDATE sessions SET revoked=1"), ErrorCode::Retryable);
    expectIdentity(*transaction, kUser, UserRole::Administrator);
    BOOST_REQUIRE(transaction->commit());
    BOOST_REQUIRE(writer->beginWrite());
    BOOST_REQUIRE(writer->execute("UPDATE sessions SET revoked=1"));
    BOOST_REQUIRE(writer->commit());
    auto next = begin(*store);
    errorIs(next->authorization().requireActiveSession({ kDigest, CredentialMode::Bearer }, kNow), ErrorCode::Unauthenticated);
}

BOOST_AUTO_TEST_CASE(read_repositories_use_the_same_connection_and_destructor_rolls_back) {
    Database database;
    database.seed();
    auto connection = database.raw();
    auto observer = database.raw();
    bool active = false;
    {
        SqliteMetadataTransaction transaction(*connection, active, { 8192, 1, 1 });
        BOOST_REQUIRE(transaction.begin());
        BOOST_CHECK(active);
        BOOST_REQUIRE(connection->execute("UPDATE sessions SET user_id=" + quoted(kMember)));
        expectIdentity(transaction, kMember, UserRole::Member);
        // 同连接内的新空间立即可读，独立连接不可见；同时覆盖析构回滚真实写入。
        BOOST_REQUIRE(connection->execute("INSERT INTO spaces VALUES (" + quoted(kMissing) + ", 0, " + quoted(kMember) + ", " + quoted(kMissing) + ")"));
        BOOST_REQUIRE(connection->execute("INSERT INTO nodes(id,space_id,parent_id,name,kind,created_by,blob_id,revision,trashed,created_at,updated_at) VALUES (" + quoted(kMissing) + ", " + quoted(kMissing) + ", NULL, '', 0, " + quoted(kMember) + ", NULL, 1, 0, 0, 0)"));
        auto space = transaction.spaces().find(SpaceId::parse(kMissing).value());
        BOOST_REQUIRE(space && space.value());
        BOOST_TEST(scalar(*observer, "SELECT count(*) FROM spaces") == 2);
    }
    BOOST_CHECK(!active);
    BOOST_CHECK(!connection->inTransaction());
    BOOST_TEST(scalar(*observer, "SELECT count(*) FROM spaces") == 2);
    SqliteMetadataTransaction next(*connection, active, { 8192, 1, 1 });
    BOOST_REQUIRE(next.begin());
    expectIdentity(next, kUser, UserRole::Administrator);
    BOOST_REQUIRE(next.commit());
}

BOOST_AUTO_TEST_CASE(commit_persists_and_constraint_failure_closes_transaction) {
    Database database;
    database.seed();
    auto connection = database.raw();
    bool active = false;
    {
        SqliteMetadataTransaction transaction(*connection, active, { 8192, 1, 1 });
        BOOST_REQUIRE(transaction.begin());
        BOOST_REQUIRE(connection->execute("UPDATE sessions SET user_id=" + quoted(kMember)));
        BOOST_REQUIRE(transaction.commit());
    }
    { auto observer = database.store(); auto transaction = begin(*observer); expectIdentity(*transaction, kMember, UserRole::Member); }
    SqliteMetadataTransaction failed(*connection, active, { 8192, 1, 1 });
    BOOST_REQUIRE(failed.begin());
    BOOST_REQUIRE(connection->execute("UPDATE users SET enabled=0 WHERE id=" + quoted(kMember)));
    BOOST_REQUIRE(connection->execute("INSERT INTO spaces VALUES (" + quoted(kMissing) + ", 0, " + quoted(kMember) + ", " + quoted(kMissing) + ")"));
    errorIs(failed.commit(), ErrorCode::IntegrityError);
    BOOST_CHECK(!active);
    errorIs(failed.authorization().requireActiveSession({ kDigest, CredentialMode::Bearer }, kNow), ErrorCode::InvalidArgument);
    errorIs(failed.commit(), ErrorCode::InvalidArgument);
    BOOST_TEST(scalar(*connection, "SELECT enabled FROM users WHERE id=" + quoted(kMember)) == 1);
    SqliteMetadataTransaction next(*connection, active, { 8192, 1, 1 });
    BOOST_REQUIRE(next.begin());
    BOOST_REQUIRE(next.commit());
}

BOOST_AUTO_TEST_CASE(corrupt_authentication_values_are_integrity_errors) {
    const std::string changes[] = {
        "UPDATE users SET role=7 WHERE id=" + quoted(kUser),
        "UPDATE users SET enabled=7 WHERE id=" + quoted(kUser),
        "UPDATE users SET auth_epoch=-1 WHERE id=" + quoted(kUser),
        "UPDATE sessions SET revoked=7",
        "UPDATE sessions SET expires_at='invalid'",
        "UPDATE sessions SET auth_epoch=-1",
        "DELETE FROM users WHERE id=" + quoted(kUser)
    };
    for (const auto& change : changes) {
        Database database;
        database.seed();
        auto store = database.store();
        auto connection = database.raw();
        // 仅测试故意制造持久化损坏；先打开 store，避免启动外键检查提前拒绝坏库。
        BOOST_REQUIRE(connection->execute("PRAGMA ignore_check_constraints=ON; PRAGMA foreign_keys=OFF; DROP TRIGGER users_identity_update; DROP TRIGGER users_delete"));
        BOOST_REQUIRE(connection->execute(change));
        auto transaction = begin(*store);
        errorIs(transaction->authorization().requireActiveSession({ kDigest, CredentialMode::Bearer }, kNow), ErrorCode::IntegrityError);
        errorIs(transaction->commit(), ErrorCode::InternalError);
    }
}

BOOST_AUTO_TEST_CASE(corrupt_space_structure_is_not_returned_as_a_valid_space) {
    const char* changes[] = {
        "UPDATE spaces SET kind=7",
        "UPDATE spaces SET owner_id=NULL WHERE kind=0",
        "UPDATE spaces SET owner_id='00000000-0000-4000-8000-000000000001' WHERE kind=1",
        "UPDATE spaces SET root_node_id='invalid'"
    };
    for (const auto* change : changes) {
        Database database;
        database.seed();
        auto store = database.store();
        auto connection = database.raw();
        BOOST_REQUIRE(connection->execute("PRAGMA ignore_check_constraints=ON; PRAGMA foreign_keys=OFF; DROP TRIGGER spaces_identity_update"));
        // kind=7 会使两个空间同时落入相同值，但公共唯一索引只覆盖 kind=1。
        BOOST_REQUIRE(connection->execute(change));
        auto transaction = begin(*store);
        const bool publicOwner = std::string_view(change).find("WHERE kind=1") != std::string_view::npos;
        errorIs(transaction->spaces().find(SpaceId::parse(publicOwner ? kPublic : kPrivate).value()), ErrorCode::IntegrityError);
    }
}

BOOST_AUTO_TEST_CASE(automatic_rollback_cannot_fall_through_to_implicit_reads_or_commit) {
    Database database;
    database.seed();
    auto connection = database.raw();
    bool active = false;
    auto old = std::make_unique<SqliteMetadataTransaction>(*connection, active, PasswordHashLimits{ 8192, 1, 1 });
    BOOST_REQUIRE(old->begin());
    errorIs(connection->execute("INSERT OR ROLLBACK INTO users SELECT * FROM users WHERE id=" + quoted(kUser)), ErrorCode::IntegrityError);
    BOOST_CHECK(!connection->inTransaction());
    errorIs(old->authorization().requireActiveSession({ kDigest, CredentialMode::Bearer }, kNow), ErrorCode::InternalError);
    errorIs(old->commit(), ErrorCode::InternalError);
    SqliteMetadataTransaction next(*connection, active, { 8192, 1, 1 });
    BOOST_REQUIRE(next.begin());
    old.reset();
    expectIdentity(next, kUser, UserRole::Administrator);
    BOOST_REQUIRE(next.commit());
}

BOOST_AUTO_TEST_CASE(commit_failure_closes_repositories_and_preserves_uncertain_result) {
    for (bool denyRollback : { false, true }) {
        Database database;
        database.seed();
        auto connection = database.raw();
        bool active = false;
        SqliteMetadataTransaction transaction(*connection, active, { 8192, 1, 1 });
        BOOST_REQUIRE(transaction.begin());
        auto denyEnd = [](void* state, int operation, const char* first, const char*, const char*, const char*) -> int {
            if (operation != SQLITE_TRANSACTION || !first) return SQLITE_OK;
            const std::string_view name(first);
            return name == "COMMIT" || (name == "ROLLBACK" && *static_cast<bool*>(state)) ? SQLITE_DENY : SQLITE_OK;
        };
        BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(connection->handle(), denyEnd, &denyRollback), SQLITE_OK);
        errorIs(transaction.commit(), denyRollback ? ErrorCode::CommitOutcomeUnknown : ErrorCode::InternalError);
        BOOST_CHECK(!active);
        errorIs(transaction.spaces().find(SpaceId::parse(kPublic).value()), ErrorCode::InvalidArgument);
        errorIs(transaction.commit(), ErrorCode::InvalidArgument);
        BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(connection->handle(), nullptr, nullptr), SQLITE_OK);
        SqliteMetadataTransaction next(*connection, active, { 8192, 1, 1 });
        if (denyRollback) errorIs(next.begin(), ErrorCode::InternalError);
        else { BOOST_REQUIRE(next.begin()); BOOST_REQUIRE(next.commit()); }
    }
}

BOOST_AUTO_TEST_CASE(closed_quota_and_audit_repositories_cannot_use_a_successor_transaction) {
    Database database;
    database.seed();
    auto store = database.store();
    const auto space = SpaceId::parse(kPublic).value();
    const auto root = NodeId::parse(kPublicRoot).value();
    auto old = begin(*store);
    auto& quotas = old->quotas();
    auto& audit = old->directoryAudit();
    BOOST_REQUIRE(old->commit());
    auto next = begin(*store);
    errorIs(quotas.chargeUsed(SpaceQuota{ space }, { ByteCount{}, NodeCount::from(1).value() }), ErrorCode::InvalidArgument);
    errorIs(audit.append({ UserId::parse(kUser).value(), space, NodeId::parse(kMissing).value(), root,
                          RequestId::parse(kMissing).value(), kNow }), ErrorCode::InvalidArgument);
    old.reset();
    BOOST_REQUIRE(next->commit());
    auto connection = database.raw();
    BOOST_TEST(scalar(*connection, "SELECT count(*) FROM audit_events") == 0);
}

}  // namespace anof::infrastructure::sqlite
