#include <anof/infrastructure/persistence/sqlite/tests/SqliteTestStore.h>
#define BOOST_TEST_MODULE AnofSqliteNodeRepository
#include <boost/test/unit_test.hpp>

#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>

#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace anof::infrastructure::sqlite {
namespace {

template <typename T>
T id(unsigned value) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setfill('0') << std::setw(12) << value;
    return T::parse(text.str()).value();
}

template <typename T>
void errorIs(const Result<T>& result, ErrorCode code) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
}

const auto kUser = id<UserId>(1);
const auto kSpace = id<SpaceId>(2);
const auto kRoot = id<NodeId>(3);
const auto kBlob = id<BlobId>(4);
const auto kOtherSpace = id<SpaceId>(6);
const auto kOtherRoot = id<NodeId>(7);
const UtcTime kNow{ std::chrono::microseconds{ 1234567 } };

template <typename T>
std::string quoted(T value) { return "'" + value.toString() + "'"; }

struct Database {
    Database() {
        char pattern[] = "/tmp/anof-sqlite-node-XXXXXX";
        const char* path = mkdtemp(pattern);
        if (!path) throw std::runtime_error("Cannot create test directory");
        directory = path;
        BOOST_REQUIRE(createSqliteDatabase(file(), { 0 }));
        auto opened = openSqliteDatabase(file(), { 0 });
        BOOST_REQUIRE(opened);
        connection = std::move(opened).value();
        BOOST_REQUIRE(connection->beginWrite());
        sql("INSERT INTO users VALUES (" + quoted(kUser) + ", 1, 1, 0, 'admin', 'Administrator', '" + std::string("$argon2id$v=19$m=8192,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") + "', 1, 1, 0, 0)");
        sql("INSERT INTO blobs(id,size,sha256,state,created_at,updated_at,revision) VALUES (" + quoted(kBlob) + ", 0, zeroblob(32),0,0,0,1)");
        sql("INSERT INTO spaces VALUES (" + quoted(kSpace) + ", 1, NULL, " + quoted(kRoot) + ")");
        sql("INSERT INTO spaces VALUES (" + quoted(kOtherSpace) + ", 0, " + quoted(kUser) + ", " + quoted(kOtherRoot) + ")");
        node(kRoot, std::nullopt, "");
        node(kOtherRoot, std::nullopt, "", false, false, kOtherSpace);
        BOOST_REQUIRE(connection->commit());
    }
    ~Database() {
        transaction.reset();
        connection.reset();
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    std::filesystem::path file() const { return directory / "metadata.sqlite3"; }
    void sql(const std::string& text) { BOOST_REQUIRE(connection->execute(text)); }
    std::int64_t scalar(const std::string& text) {
        auto statement = SqliteStatement::prepare(*connection, text);
        BOOST_REQUIRE(statement);
        auto row = statement.value()->step();
        BOOST_REQUIRE(row && row.value());
        auto value = statement.value()->integer(0);
        BOOST_REQUIRE(value);
        return value.value();
    }
    void node(NodeId nodeId, std::optional<NodeId> parent, std::string_view name,
              bool fileNode = false, bool trashed = false, SpaceId space = kSpace) {
        auto statement = SqliteStatement::prepare(*connection,
                                                  "INSERT INTO nodes(id,space_id,parent_id,name,kind,created_by,blob_id,revision,trashed,created_at,updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 1, ?8, 0, 0)");
        BOOST_REQUIRE(statement);
        auto& s = *statement.value();
        BOOST_REQUIRE(s.bindText(1, nodeId.toString()));
        BOOST_REQUIRE(s.bindText(2, space.toString()));
        if (parent) BOOST_REQUIRE(s.bindText(3, parent->toString()));
        else BOOST_REQUIRE(s.bindNull(3));
        BOOST_REQUIRE(s.bindText(4, name));
        BOOST_REQUIRE(s.bindInt64(5, fileNode ? 1 : 0));
        BOOST_REQUIRE(s.bindText(6, kUser.toString()));
        if (fileNode) BOOST_REQUIRE(s.bindText(7, kBlob.toString()));
        else BOOST_REQUIRE(s.bindNull(7));
        BOOST_REQUIRE(s.bindInt64(8, trashed ? 1 : 0));
        auto row = s.step();
        BOOST_REQUIRE(row && !row.value());
    }
    NodeRepository& start() {
        transaction.reset();
        transaction = std::make_unique<SqliteMetadataTransaction>(*connection, active, PasswordHashLimits{ 8192, 1, 1 });
        BOOST_REQUIRE(transaction->begin());
        return transaction->nodes();
    }
    NewDirectory command(unsigned number = 10, std::string name = "child", NodeId parent = kRoot) {
        return { id<NodeId>(number), kSpace, parent, std::move(name), kUser, kNow };
    }
    std::unique_ptr<MetadataStore> store() {
        auto result = testing::openTestMetadataStore(file(), { 0 });
        BOOST_REQUIRE(result);
        return std::move(result).value();
    }
    std::filesystem::path directory;
    std::unique_ptr<SqliteConnection> connection;
    bool active = false;
    std::unique_ptr<SqliteMetadataTransaction> transaction;
};

}  // namespace

BOOST_AUTO_TEST_CASE(insert_returns_exact_snapshot_and_path_persists_after_reopen) {
    Database db;
    auto command = db.command(10, " 家庭资料 ");
    auto& nodes = db.start();
    auto inserted = nodes.insertDirectory(command);
    BOOST_REQUIRE(inserted);
    const auto& value = inserted.value();
    BOOST_CHECK(value.id == command.id);
    BOOST_CHECK(value.spaceId == kSpace);
    BOOST_CHECK(value.parentId.value() == kRoot);
    BOOST_CHECK(value.name == command.name);
    BOOST_CHECK(value.createdBy == kUser);
    BOOST_TEST(value.revision.value() == 1);
    BOOST_CHECK(value.createdAt == kNow && value.updatedAt == kNow);
    BOOST_REQUIRE(nodes.insertDirectory(db.command(11, "nested", command.id)));
    auto path = nodes.findActiveDirectoryPath(kSpace, id<NodeId>(11));
    BOOST_REQUIRE(path);
    BOOST_REQUIRE_EQUAL(path.value().size(), 3U);
    BOOST_CHECK(path.value()[0].id == kRoot);
    BOOST_CHECK(path.value()[1].id == command.id);
    BOOST_CHECK(path.value()[2].parentId.value() == command.id);
    BOOST_TEST(path.value()[0].revision.value() == 1);
    BOOST_TEST(db.scalar("SELECT count(*) FROM space_quotas") == 0);
    BOOST_TEST(db.scalar("SELECT count(*) FROM audit_events") == 0);
    BOOST_REQUIRE(db.transaction->commit());
    auto store = db.store();
    auto tx = store->beginWrite();
    BOOST_REQUIRE(tx);
    auto persisted = tx.value()->nodes().findActiveDirectoryPath(kSpace, id<NodeId>(11));
    BOOST_REQUIRE(persisted);
    BOOST_TEST(persisted.value().size() == 3U);
    errorIs(nodes.findActiveDirectoryPath(kSpace, kRoot), ErrorCode::InvalidArgument);
    errorIs(nodes.insertDirectory(db.command(12)), ErrorCode::InvalidArgument);
    errorIs(nodes.advanceRevision(kRoot, Revision{}, kNow), ErrorCode::InvalidArgument);
}

BOOST_AUTO_TEST_CASE(insert_and_revision_roll_back_together_on_abandonment_and_failure) {
    for (bool conflict : { false, true }) {
        Database db;
        auto& nodes = db.start();
        BOOST_REQUIRE(nodes.insertDirectory(db.command()));
        auto advanced = nodes.advanceRevision(kRoot, Revision{}, kNow);
        BOOST_REQUIRE(advanced);
        BOOST_TEST(advanced.value().value() == 2);
        if (conflict) {
            errorIs(nodes.insertDirectory(db.command(11)), ErrorCode::NameConflict);
            errorIs(db.transaction->commit(), ErrorCode::InternalError);
        }
        db.transaction.reset();
        BOOST_TEST(db.scalar("SELECT count(*) FROM nodes") == 2);
        BOOST_TEST(db.scalar("SELECT revision FROM nodes WHERE id=" + quoted(kRoot)) == 1);
        BOOST_TEST(db.scalar("SELECT updated_at FROM nodes WHERE id=" + quoted(kRoot)) == 0);
    }
}

BOOST_AUTO_TEST_CASE(directory_names_validate_utf8_bytes_and_controls_without_normalization) {
    Database db;
    const std::string invalid[] = { "", ".", "..", "a/b", "a\\b", std::string("a\0b", 3), std::string(256, 'x'),
        "\x01", "\x7f", "\xc2\x85", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8" };
    for (const auto& name : invalid) errorIs(db.start().insertDirectory(db.command(10, name)), ErrorCode::InvalidName);
    auto& nodes = db.start();
    unsigned index = 10;
    for (const auto& name : { std::string(255, 'x'), std::string(" "), std::string("Case"), std::string("case"),
                             std::string("\xc3\xa9"), std::string("e\xcc\x81") }) {
        auto result = nodes.insertDirectory(db.command(index++, name));
        BOOST_REQUIRE(result);
        BOOST_CHECK(result.value().name == name);
    }
    BOOST_REQUIRE(db.transaction->commit());
}

BOOST_AUTO_TEST_CASE(name_conflicts_include_files_but_id_and_foreign_key_failures_are_integrity_errors) {
    for (bool file : { false, true }) {
        Database db;
        db.node(id<NodeId>(10), kRoot, "child", file);
        errorIs(db.start().insertDirectory(db.command(11)), ErrorCode::NameConflict);
        errorIs(db.start().insertDirectory(db.command(10)), ErrorCode::IntegrityError);
        errorIs(db.start().insertDirectory(db.command(10, "another")), ErrorCode::IntegrityError);
        auto command = db.command(12, "missing-creator");
        command.createdBy = id<UserId>(99);
        errorIs(db.start().insertDirectory(command), ErrorCode::IntegrityError);
        db.transaction.reset();
        BOOST_TEST(db.scalar("SELECT count(*) FROM nodes") == 3);
    }
}

BOOST_AUTO_TEST_CASE(missing_cross_space_and_file_parents_have_precise_errors) {
    Database db;
    db.node(id<NodeId>(10), kRoot, "file", true);
    const struct { SpaceId space; NodeId parent; ErrorCode code; } cases[] = {
        { id<SpaceId>(99), kRoot, ErrorCode::NotFound },
        { kSpace, id<NodeId>(99), ErrorCode::NotFound },
        { kSpace, kOtherRoot, ErrorCode::NotFound },
        { kSpace, id<NodeId>(10), ErrorCode::ParentNotDirectory }
    };
    for (const auto& item : cases) {
        errorIs(db.start().findActiveDirectoryPath(item.space, item.parent), item.code);
        auto command = db.command(11, "new", item.parent);
        command.spaceId = item.space;
        errorIs(db.start().insertDirectory(command), item.code);
    }
}

BOOST_AUTO_TEST_CASE(trashed_ancestor_hides_both_directory_and_file_targets) {
    Database db;
    db.node(id<NodeId>(10), kRoot, "child");
    db.node(id<NodeId>(11), id<NodeId>(10), "nested");
    db.node(id<NodeId>(12), id<NodeId>(11), "file", true);
    db.start();
    auto tree = db.transaction->tree().activeSubtree(id<NodeId>(10), {}).value();
    auto summary = summarizeTree(tree).value();
    TrashEntry entry{ id<TrashId>(10), kSpace, id<NodeId>(10), kRoot, "child", NodeKind::Directory, kUser, kNow, trashExpiry(kNow).value(), {}, summary.total.nodes, summary.total.bytes, kUser };
    BOOST_REQUIRE(db.transaction->trash().insert(entry));
    BOOST_REQUIRE(db.transaction->tree().detach(tree, { id<NodeId>(10), {} }, entry));
    BOOST_REQUIRE(db.transaction->commit());
    for (unsigned target : { 10U, 11U, 12U }) {
        errorIs(db.start().findActiveDirectoryPath(kSpace, id<NodeId>(target)), ErrorCode::NotFound);
        errorIs(db.start().insertDirectory(db.command(13, "new", id<NodeId>(target))), ErrorCode::NotFound);
    }
    BOOST_REQUIRE(db.start().insertDirectory(db.command(13, "child")));
    BOOST_REQUIRE(db.transaction->commit());
}

BOOST_AUTO_TEST_CASE(depth_128_is_readable_but_cannot_have_a_child) {
    Database db;
    BOOST_REQUIRE(db.connection->beginWrite());
    auto parent = kRoot;
    for (unsigned depth = 1; depth <= 128; ++depth) {
        const auto current = id<NodeId>(100 + depth);
        db.node(current, parent, "level");
        parent = current;
    }
    BOOST_REQUIRE(db.connection->commit());
    auto path = db.start().findActiveDirectoryPath(kSpace, parent);
    BOOST_REQUIRE(path);
    BOOST_TEST(path.value().size() == 129U);
    errorIs(db.transaction->nodes().insertDirectory(db.command(500, "too-deep", parent)), ErrorCode::DepthLimitExceeded);
    BOOST_REQUIRE(db.start().insertDirectory(db.command(500, "at-limit", id<NodeId>(227))));
    BOOST_REQUIRE(db.transaction->commit());
    db.transaction.reset();
    db.node(id<NodeId>(501), parent, "corrupt-depth");
    errorIs(db.start().findActiveDirectoryPath(kSpace, id<NodeId>(501)), ErrorCode::IntegrityError);
}

BOOST_AUTO_TEST_CASE(cycles_broken_links_and_invalid_roots_terminate_with_integrity_error) {
    const std::string changes[] = {
        "UPDATE nodes SET parent_id=" + quoted(id<NodeId>(11)) + " WHERE id=" + quoted(id<NodeId>(10)),
        "UPDATE nodes SET parent_id=" + quoted(id<NodeId>(99)) + " WHERE id=" + quoted(id<NodeId>(10)),
        "UPDATE nodes SET parent_id=NULL WHERE id=" + quoted(id<NodeId>(10)),
        "UPDATE nodes SET parent_id=" + quoted(kOtherRoot) + " WHERE id=" + quoted(id<NodeId>(10)),
        "UPDATE nodes SET name='bad-root' WHERE id=" + quoted(kRoot),
        "UPDATE nodes SET kind=1, blob_id=" + quoted(kBlob) + " WHERE id=" + quoted(id<NodeId>(10)),
        "UPDATE nodes SET revision=0 WHERE id=" + quoted(id<NodeId>(10)),
        "UPDATE nodes SET name=char(1) WHERE id=" + quoted(id<NodeId>(10))
    };
    for (const auto& change : changes) {
        Database db;
        db.node(id<NodeId>(10), kRoot, "a");
        db.node(id<NodeId>(11), id<NodeId>(10), "b");
        // 仅测试夹具绕过约束构造损坏库，实际迁移与生产写接口不放宽。
        db.sql("PRAGMA foreign_keys=OFF; PRAGMA ignore_check_constraints=ON; DROP TRIGGER nodes_shape_update; DROP TRIGGER nodes_identity_update; DROP TRIGGER nodes_blob_update; DROP INDEX nodes_active_root");
        db.sql(change);
        db.sql("PRAGMA foreign_keys=ON; PRAGMA ignore_check_constraints=OFF");
        errorIs(db.start().findActiveDirectoryPath(kSpace, id<NodeId>(11)), ErrorCode::IntegrityError);
        errorIs(db.transaction->commit(), ErrorCode::InternalError);
    }
}

BOOST_AUTO_TEST_CASE(revision_compare_increment_and_time_are_checked_at_integer_boundaries) {
    Database db;
    for (std::int64_t initial : { 1LL, 2147483647LL, 4294967295LL, 9223372036854775806LL }) {
        db.transaction.reset();
        db.sql("UPDATE nodes SET revision=" + std::to_string(initial) + " WHERE id=" + quoted(kRoot));
        auto next = db.start().advanceRevision(kRoot, Revision::from(initial).value(), kNow);
        BOOST_REQUIRE(next);
        BOOST_TEST(next.value().value() == initial + 1);
        BOOST_REQUIRE(db.transaction->commit());
        BOOST_TEST(db.scalar("SELECT updated_at FROM nodes WHERE id=" + quoted(kRoot)) == kNow.time_since_epoch().count());
    }
    errorIs(db.start().advanceRevision(kRoot, Revision{}, kNow), ErrorCode::Conflict);
    errorIs(db.start().advanceRevision(kRoot, Revision::from(std::numeric_limits<std::int64_t>::max()).value(), kNow), ErrorCode::IntegrityError);
    errorIs(db.start().advanceRevision(id<NodeId>(99), Revision{}, kNow), ErrorCode::NotFound);
    db.transaction.reset();
    BOOST_TEST(db.scalar("SELECT revision FROM nodes WHERE id=" + quoted(kRoot)) == std::numeric_limits<std::int64_t>::max());
}

BOOST_AUTO_TEST_CASE(two_stores_compete_for_same_name_without_overwrite) {
    Database db;
    auto first = db.store();
    auto second = db.store();
    auto firstTx = first->beginWrite();
    BOOST_REQUIRE(firstTx);
    BOOST_REQUIRE(firstTx.value()->nodes().insertDirectory(db.command()));
    errorIs(second->beginWrite(), ErrorCode::Retryable);
    BOOST_REQUIRE(firstTx.value()->commit());
    auto secondTx = second->beginWrite();
    BOOST_REQUIRE(secondTx);
    errorIs(secondTx.value()->nodes().insertDirectory(db.command(11)), ErrorCode::NameConflict);
    errorIs(secondTx.value()->commit(), ErrorCode::InternalError);
    BOOST_TEST(db.scalar("SELECT count(*) FROM nodes WHERE parent_id=" + quoted(kRoot)) == 1);
    BOOST_TEST(db.scalar("SELECT revision FROM nodes WHERE id=" + quoted(kRoot)) == 1);
}

BOOST_AUTO_TEST_CASE(writes_detect_zero_affected_rows_and_do_not_return_success) {
    Database db;
    // 条件更新影响零行也必须报告冲突；触发器仅属于测试注入。
    db.sql("CREATE TRIGGER test_skip_revision BEFORE UPDATE OF revision ON nodes BEGIN SELECT RAISE(IGNORE); END");
    errorIs(db.start().advanceRevision(kRoot, Revision{}, kNow), ErrorCode::Conflict);
    errorIs(db.transaction->commit(), ErrorCode::InternalError);
    BOOST_TEST(db.scalar("SELECT revision FROM nodes WHERE id=" + quoted(kRoot)) == 1);
    db.sql("CREATE TRIGGER test_skip_insert BEFORE INSERT ON nodes BEGIN SELECT RAISE(IGNORE); END");
    errorIs(db.start().insertDirectory(db.command()), ErrorCode::IntegrityError);
    errorIs(db.transaction->commit(), ErrorCode::InternalError);
    BOOST_TEST(db.scalar("SELECT count(*) FROM nodes") == 2);
}

BOOST_AUTO_TEST_CASE(node_event_time_uses_persistent_lower_bound) {
    Database db;
    db.sql("UPDATE nodes SET created_at=100,updated_at=100");
    BOOST_REQUIRE(db.start().advanceRevision(kRoot, Revision{}, UtcTime{ std::chrono::microseconds{ 200 } }));
    db.transaction.reset();
    BOOST_CHECK_EQUAL(db.scalar("SELECT updated_at FROM nodes WHERE id=" + quoted(kRoot)), 100);
    BOOST_CHECK_EQUAL(db.scalar("SELECT revision FROM nodes WHERE id=" + quoted(kRoot)), 1);
    std::int64_t revision = 1, expectedTime = 100;
    for (auto observed : { INT64_MIN, std::int64_t{ 100 }, std::int64_t{ 120 }, std::int64_t{ 110 }, std::int64_t{ 120 }, INT64_MAX }) {
        expectedTime = std::max(expectedTime, observed);
        auto& nodes = db.start();
        auto next = nodes.advanceRevision(kRoot, Revision::from(revision).value(), UtcTime{ std::chrono::microseconds{ observed } });
        BOOST_REQUIRE(next);
        BOOST_CHECK_EQUAL(next.value().value(), ++revision);
        auto node = nodes.find(kRoot);
        BOOST_REQUIRE(node && node.value());
        BOOST_CHECK_EQUAL(node.value()->metadata.updatedAt.time_since_epoch().count(), expectedTime);
        BOOST_REQUIRE(db.transaction->commit());
        db.transaction.reset();
        db.connection.reset();
        db.connection = std::move(openSqliteDatabase(db.file(), { 0 })).value();
        BOOST_CHECK_EQUAL(db.scalar("SELECT updated_at FROM nodes WHERE id=" + quoted(kRoot)), expectedTime);
    }
    errorIs(db.start().advanceRevision(kRoot, Revision{}, kNow), ErrorCode::Conflict);
    BOOST_CHECK(!db.transaction->commit());
    BOOST_REQUIRE(db.start().advanceRevision(kRoot, Revision::from(revision).value(), kNow));
    db.transaction.reset();
    BOOST_CHECK_EQUAL(db.scalar("SELECT revision FROM nodes WHERE id=" + quoted(kRoot)), revision);
    BOOST_CHECK_EQUAL(db.scalar("SELECT updated_at FROM nodes WHERE id=" + quoted(kRoot)), INT64_MAX);
}

}  // namespace anof::infrastructure::sqlite
