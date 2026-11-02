#define BOOST_TEST_MODULE AnofM2Boundaries
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <anof/infrastructure/persistence/sqlite/tests/BootstrapFixture.h>
#include <anof/infrastructure/system/LinuxInstanceInitializer.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>
#include <anof/infrastructure/system/internal/InitializationFiles.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/test/unit_test.hpp>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <future>
#include <stdexcept>

namespace anof::infrastructure::sqlite {
namespace {
using testing::bootstrapBudget;
using testing::bootstrapId;
using testing::bootstrapSeed;
namespace files = system::detail;
void require(bool valid) {
    if (!valid) throw std::runtime_error("boundary check failed");
}
template <typename T>
void require(const T& result) { require(static_cast<bool>(result)); }
struct Tree {
    std::filesystem::path root;
    Tree() {
        char name[] = "/tmp/anof-m2-boundary-XXXXXX";
        const auto* path = ::mkdtemp(name);
        require(path != nullptr);
        root = path;
    }
    ~Tree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path path(const char* name) const { return root / ".anof" / name; }
};
std::unique_ptr<SqliteConnection> open(const std::filesystem::path& path) {
    auto db = SqliteConnection::open(path, { 0 }, SqliteOpenMode::Inspect);
    require(db);
    return std::move(db).value();
}
std::int64_t scalar(SqliteConnection& db, const char* sql) {
    auto q = SqliteStatement::prepare(db, sql);
    require(q);
    auto row = q.value()->step();
    require(row && row.value());
    auto value = q.value()->integer(0);
    require(value);
    return value.value();
}
InstanceConfiguration config() {
    InstanceConfiguration result;
    result.developmentHttp = true;
    return result;
}
NewUser member(unsigned number, std::string_view login) {
    auto seed = bootstrapSeed();
    return { { bootstrapId<UserId>(number), LoginName::parse(login).value(), DisplayName::parse("Member").value(),
               UserRole::Member, true, AuthEpoch{}, Revision{}, seed.createdAt, seed.createdAt },
             std::move(seed.encodedPassword),
             PasswordVersion{} };
}
NewSession session(unsigned number) {
    SessionDigest::Bytes bytes{};
    bytes[0] = static_cast<std::uint8_t>(number);
    auto seed = bootstrapSeed();
    return { SessionDigest{ bytes }, seed.adminUserId, AuthEpoch{}, CredentialMode::Bearer, seed.createdAt, seed.createdAt + std::chrono::hours(1) };
}
InviteDigest inviteDigest() { return InviteDigest{ InviteDigest::Bytes{ 1 } }; }
struct Gate {
    std::promise<void> promise;
    std::shared_future<void> signal = promise.get_future().share();
    void release() { promise.set_value(); }
    void wait() {
        require(signal.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        signal.get();
    }
};
enum class Race { Login,
                  Invite,
                  UserLimit,
                  SessionLimit };
void compete(Race race, bool rollbackFirst = false) {
    Tree tree;
    const auto path = tree.root / "db";
    testing::createBootstrappedTestDatabase(path);
    auto seed = bootstrapSeed();
    {
        auto db = open(path);
        bool active = false;
        SqliteMetadataTransaction tx(*db, active, bootstrapBudget);
        require(tx.begin());
        if (race == Race::Invite) require(tx.invites().insert({ { bootstrapId<InviteId>(1), seed.adminUserId, seed.createdAt, seed.createdAt + std::chrono::hours(1), InviteUseLimit::from(1).value(), InviteUseCount{}, {} }, inviteDigest() }));
        if (race == Race::SessionLimit)
            for (unsigned i = 0; i < 31; ++i) require(tx.sessions().insert(session(i)));
        require(tx.commit());
        if (race == Race::UserLimit) require(db->execute("UPDATE instance_settings SET max_users=2"));
    }
    Gate written, attempted, committed;
    auto action = [&](SqliteMetadataTransaction& tx, bool second) {
        const bool wins = !second || rollbackFirst;
        if (race == Race::Login) {
            auto inserted = tx.users().insert(member(second ? 9 : 8, second ? "shared" : "SHARED"));
            if (wins)
                require(inserted);
            else
                require(!inserted && inserted.error().code == ErrorCode::LoginConflict);
        } else if (race == Race::Invite) {
            auto consumed = tx.invites().consume(inviteDigest(), seed.createdAt);
            if (wins) {
                require(consumed);
                require(tx.users().insert(member(second ? 9 : 8, second ? "second" : "first")));
                require(tx.identityAudit().append({ bootstrapId<RequestId>(second ? 9 : 8), seed.createdAt,
                                                    UserRegistered{ bootstrapId<UserId>(second ? 9 : 8), bootstrapId<InviteId>(1) } }));
            } else
                require(!consumed && consumed.error().code == ErrorCode::InviteUnavailable);
        } else if (race == Race::UserLimit) {
            auto count = tx.users().countUsers();
            auto settings = tx.settings().get();
            require(count);
            require(settings && settings.value());
            require((count.value().value() < settings.value()->maxUsers.value()) == wins);
            if (wins) require(tx.users().insert(member(second ? 9 : 8, second ? "second" : "first")));
        } else {
            auto count = tx.sessions().countActive(seed.adminUserId, seed.createdAt);
            require(count);
            require((count.value().value() < kMaxActiveSessions) == wins);
            if (wins) {
                require(tx.sessions().insert(session(second ? 33 : 32)));
                require(tx.identityAudit().append({ bootstrapId<RequestId>(second ? 9 : 8), seed.createdAt, SessionCreated{ seed.adminUserId } }));
            }
        }
    };
    // 同步点仅用于测试制造确定的重叠；生产不得持事务等待其他 worker。
    auto first = std::async(std::launch::async, [&] {
        auto db = open(path);
        bool active = false;
        {
            SqliteMetadataTransaction tx(*db, active, bootstrapBudget);
            require(tx.begin());
            action(tx, false);
            written.release();
            attempted.wait();
            if (!rollbackFirst) require(tx.commit());
        }
        committed.release();
    });
    auto second = std::async(std::launch::async, [&] {
        written.wait();
        auto db = open(path);
        bool active = false;
        {
            SqliteMetadataTransaction blocked(*db, active, bootstrapBudget);
            auto begun = blocked.begin();
            require(!begun && begun.error().code == ErrorCode::Retryable);
        }
        attempted.release();
        committed.wait();
        SqliteMetadataTransaction tx(*db, active, bootstrapBudget);
        require(tx.begin());
        action(tx, true);
        if (rollbackFirst || race == Race::UserLimit || race == Race::SessionLimit) require(tx.commit());
    });
    first.get();
    second.get();
    auto db = open(path);
    BOOST_CHECK_EQUAL(scalar(*db, "SELECT count(*) FROM users"), race == Race::SessionLimit ? 1 : 2);
    if (race == Race::Invite) {
        BOOST_CHECK_EQUAL(scalar(*db, "SELECT used_count FROM invites"), 1);
        BOOST_CHECK_EQUAL(scalar(*db, "SELECT count(*) FROM identity_audit_events WHERE action='user_registered'"), 1);
        BOOST_CHECK_EQUAL(scalar(*db, "SELECT count(*) FROM users WHERE login='first'"), rollbackFirst ? 0 : 1);
    }
    if (race == Race::SessionLimit) {
        BOOST_CHECK_EQUAL(scalar(*db, "SELECT count(*) FROM sessions"), 32);
        BOOST_CHECK_EQUAL(scalar(*db, "SELECT count(*) FROM identity_audit_events WHERE action='session_created'"), 1);
    }
}

// 有限 pipe 等待用于跨进程控制，所有描述符在对象析构时关闭。
struct Pipe {
    files::OwnedFd read, write;
    Pipe() {
        int fds[2];
        require(::pipe2(fds, O_CLOEXEC) == 0);
        read = files::OwnedFd(fds[0]);
        write = files::OwnedFd(fds[1]);
    }
    void send(char value) { require(::write(write.get(), &value, 1) == 1); }
    char receive() {
        struct pollfd item{ read.get(), POLLIN, 0 };
        int ready;
        do {
            ready = ::poll(&item, 1, 5000);
        } while (ready < 0 && errno == EINTR);
        require(ready > 0);
        char value = 0;
        require(::read(read.get(), &value, 1) == 1);
        return value;
    }
};
class Child {
public:
    template <typename Function>
    explicit Child(Function function) {
        pid_ = ::fork();
        require(pid_ >= 0);
        if (pid_ == 0) {
            ::alarm(8);
            try {
                function();
                ::_exit(0);
            } catch (...) {
                ::_exit(90);
            }
        }
    }
    ~Child() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    void join() {
        int status = 0;
        pid_t result;
        do {
            result = ::waitpid(pid_, &status, 0);
        } while (result < 0 && errno == EINTR);
        require(result == pid_);
        pid_ = -1;
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

private:
    pid_t pid_ = -1;
};
int stopPoint = 0;
int renameHook(int from, const char* source, int to, const char* target) {
    if (stopPoint == 3 && std::strcmp(target, "instance.json") == 0) ::_exit(0);
    const int result = files::initializationFileOps().renameNoReplace(from, source, to, target);
    if (result == 0 && ((stopPoint == 1 && std::strcmp(target, "initialization.pending") == 0) ||
                        (stopPoint == 2 && std::strcmp(target, "config.toml") == 0) || (stopPoint == 4 && std::strcmp(target, "instance.json") == 0))) ::_exit(0);
    return result;
}
int unlinkHook(int directory, const char* name, int flags) {
    if (stopPoint == 5) ::_exit(0);
    const int result = ::unlinkat(directory, name, flags);
    if (stopPoint == 6 && result == 0) ::_exit(0);
    return result;
}
std::unique_ptr<InitializationLease> createPending(const Tree& tree) {
    auto init = system::makeLinuxInstanceInitializer();
    require(init);
    auto lease = init.value()->create(tree.root, bootstrapSeed().identity);
    require(lease);
    require(lease.value()->writeConfiguration(config()));
    return std::move(lease).value();
}
std::unique_ptr<InstanceBootstrap> bootstrap() {
    auto result = makeSqliteInstanceBootstrap({ bootstrapBudget, { 0 } });
    require(result);
    return std::move(result).value();
}
std::unique_ptr<InstanceMaintenance> maintenance() {
    auto result = makeSqliteInstanceMaintenance({ bootstrapBudget, { 0 } });
    require(result);
    return std::move(result).value();
}
void initialCounts(const Tree& tree) {
    auto db = open(tree.path("metadata.sqlite3"));
    BOOST_CHECK_EQUAL(scalar(*db, "SELECT count(*) FROM users"), 1);
    BOOST_CHECK_EQUAL(scalar(*db, "SELECT count(*) FROM identity_audit_events"), 1);
    BOOST_CHECK_EQUAL(scalar(*db, "SELECT sum(used_nodes) FROM space_quotas"), 2);
    BOOST_CHECK_EQUAL(scalar(*db, "SELECT sum(used_nodes) FROM public_contribution_quotas"), 1);
}
}  // namespace

BOOST_AUTO_TEST_CASE(two_workers_normalized_login_race) { compete(Race::Login); }
BOOST_AUTO_TEST_CASE(two_workers_last_invite_race) { compete(Race::Invite); }
BOOST_AUTO_TEST_CASE(two_workers_last_user_slot_race) { compete(Race::UserLimit); }
BOOST_AUTO_TEST_CASE(two_workers_last_session_slot_race) { compete(Race::SessionLimit); }
BOOST_AUTO_TEST_CASE(rolled_back_registration_releases_invite_and_audit_atomically) { compete(Race::Invite, true); }

BOOST_AUTO_TEST_CASE(process_exit_at_publication_boundaries_preserves_completion_evidence) {
    for (int point = 1; point <= 6; ++point) {
        BOOST_TEST_CONTEXT("publication boundary " << point) {
            Tree tree;
            Child child([&] {
                stopPoint = point;
                auto ops = files::initializationFileOps();
                ops.renameNoReplace = renameHook;
                ops.unlink = unlinkHook;
                auto init = files::makeLinuxInstanceInitializerWith(ops);
                require(init);
                auto seed = bootstrapSeed();
                auto lease = init.value()->create(tree.root, seed.identity);
                require(lease);
                require(lease.value()->writeConfiguration(config()));
                auto backend = bootstrap();
                require(backend->createCurrent(lease.value()->lease(), TransferBootstrapSeed{ bootstrapSeed(), ByteCount::from(1024).value() }));
                require(backend->verifyComplete(lease.value()->lease(), seed.identity));
                require(lease.value()->publishFormat());
                require(lease.value()->clearPending());
                ::_exit(91);  // 必须由指定边界终止，不能误把完整执行当覆盖。
            });
            child.join();
            struct stat lock{};
            require(::stat(tree.path("instance.lock").c_str(), &lock) == 0);
            auto init = system::makeLinuxInstanceInitializer();
            require(init);
            auto lease = init.value()->openPending(tree.root);
            if (point == 1 || point == 6) {
                BOOST_REQUIRE(!lease);
                BOOST_CHECK(lease.error().code == InitError::IncompleteInitialization);
            } else {
                BOOST_REQUIRE(lease);
                auto verified = bootstrap()->verifyComplete(lease.value()->lease(), bootstrapSeed().identity);
                if (point == 2) {
                    BOOST_REQUIRE(!verified);
                    BOOST_CHECK(verified.error().code == InitError::IncompleteInitialization);
                    BOOST_CHECK(!std::filesystem::exists(tree.path("metadata.sqlite3")));
                } else {
                    BOOST_REQUIRE(verified);
                    BOOST_CHECK(!maintenance()->verify(lease.value()->lease()));
                    require(lease.value()->publishFormat());
                    require(lease.value()->clearPending());
                    require(maintenance()->verify(lease.value()->lease()));
                    initialCounts(tree);
                }
            }
            lease = InitFailure{ InitError::IncompleteInitialization, InitStage::Verify };  // 释放锁后独立重开。
            auto locks = system::makeLinuxInstanceLockProvider();
            require(locks);
            auto held = locks.value()->acquire(tree.root);
            require(held);
            auto checked = maintenance()->verify(*held.value());
            BOOST_CHECK(static_cast<bool>(checked) == (point >= 3));
            struct stat after{};
            require(::stat(tree.path("instance.lock").c_str(), &after) == 0);
            BOOST_CHECK_EQUAL(lock.st_ino, after.st_ino);
            if (point == 6) initialCounts(tree);
        }
    }
}

BOOST_AUTO_TEST_CASE(simultaneous_creators_have_one_owner_without_replacing_lock) {
    Tree tree;
    Pipe start, ready, release;
    auto run = [&] {
        start.receive();
        auto init = system::makeLinuxInstanceInitializer();
        require(init);
        auto lease = init.value()->create(tree.root, bootstrapSeed().identity);
        if (lease) {
            ready.send('W');
            release.receive();
        } else {
            require(lease.error().code == InitError::AlreadyExists);
            ready.send('L');
        }
    };
    Child first(run);
    Child second(run);
    start.send('G');
    start.send('G');
    const auto a = ready.receive(), b = ready.receive();
    BOOST_CHECK((a == 'W' && b == 'L') || (a == 'L' && b == 'W'));
    auto locks = system::makeLinuxInstanceLockProvider();
    require(locks);
    auto held = locks.value()->acquire(tree.root);
    BOOST_REQUIRE(!held);
    BOOST_CHECK(held.error().code == ErrorCode::Conflict);
    release.send('G');
    first.join();
    second.join();
    require(locks.value()->acquire(tree.root));
    BOOST_CHECK(std::filesystem::exists(tree.path("initialization.pending")));
}

BOOST_AUTO_TEST_CASE(finisher_excludes_other_finish_and_serve_until_lease_release) {
    Tree tree;
    {
        auto lease = createPending(tree);
        require(bootstrap()->createCurrent(lease->lease(), TransferBootstrapSeed{ bootstrapSeed(), ByteCount::from(1024).value() }));
    }
    Pipe ready, release;
    Child child([&] {
        auto init = system::makeLinuxInstanceInitializer();
        require(init);
        auto lease = init.value()->openPending(tree.root);
        require(lease);
        require(bootstrap()->verifyComplete(lease.value()->lease(), lease.value()->identity()));
        ready.send('G');
        release.receive();
        require(lease.value()->publishFormat());
        require(lease.value()->clearPending());
        ready.send('G');
        release.receive();
    });
    ready.receive();
    auto init = system::makeLinuxInstanceInitializer();
    require(init);
    auto competing = init.value()->openPending(tree.root);
    BOOST_REQUIRE(!competing);
    BOOST_CHECK(competing.error().code == InitError::Busy);
    auto locks = system::makeLinuxInstanceLockProvider();
    require(locks);
    auto held = locks.value()->acquire(tree.root);
    BOOST_REQUIRE(!held);
    BOOST_CHECK(held.error().code == ErrorCode::Conflict);
    release.send('G');
    ready.receive();
    competing = init.value()->openPending(tree.root);
    BOOST_REQUIRE(!competing);
    BOOST_CHECK(competing.error().code == InitError::Busy);
    release.send('G');
    child.join();
    competing = init.value()->openPending(tree.root);
    BOOST_REQUIRE(!competing);
    BOOST_CHECK(competing.error().code == InitError::IncompleteInitialization);
    held = locks.value()->acquire(tree.root);
    BOOST_REQUIRE(held);
    require(maintenance()->verify(*held.value()));
    initialCounts(tree);
}
}  // namespace anof::infrastructure::sqlite
