#define BOOST_TEST_MODULE SqliteIdentity
#include <boost/test/unit_test.hpp>
#include <anof/infrastructure/persistence/sqlite/SqliteAuthorizationReader.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUserRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSessionRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInviteRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSettingsRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentityAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStatement.h>
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace anof::infrastructure::sqlite {
namespace {
constexpr PasswordHashLimits limits{ 8192, 1, 1 };
constexpr const char* hashText = "$argon2id$v=19$m=8192,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
UtcTime time(std::int64_t n) {
    return UtcTime(std::chrono::microseconds(n));
}

template <class T>

T id(unsigned n) {
    char text[37];
    std::snprintf(text, sizeof(text), "00000000-0000-4000-8000-%012x", n);
    return T::parse(text).value();
}

template <class T>

void errorIs(const Result<T>& result, ErrorCode code) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
}

EncodedPasswordHash hash() {
    return std::move(EncodedPasswordHash::parse(hashText, limits)).value();
}

NewUser newUser(unsigned n, std::string_view login = "member", UserRole role = UserRole::Member) {
    return {{id<UserId>(n), LoginName::parse(login).value(), DisplayName::parse("Member").value(), role, true, AuthEpoch{}, Revision{}, time(0), time(0) }, hash(), PasswordVersion{} };
}

SessionDigest digest(unsigned n) {
    SessionDigest::Bytes bytes{};
    bytes[0] = static_cast<std::uint8_t>(n);
    return SessionDigest(bytes);
}

InviteDigest inviteDigest(unsigned n) {
    InviteDigest::Bytes bytes{};
    bytes[0] = static_cast<std::uint8_t>(n);
    return InviteDigest(bytes);
}

NewInvite newInvite(unsigned n, std::int64_t maximum = 1, std::int64_t created = 0, std::int64_t expires = 100) {
    return {{id<InviteId>(n), id<UserId>(1), time(created), time(expires), InviteUseLimit::from(maximum).value(), InviteUseCount{}, {} }, inviteDigest(n) };
}

NewSession newSession(unsigned n, unsigned user = 2, std::int64_t epoch = 0, std::int64_t expires = 100) {
    return {digest(n), id<UserId>(user), AuthEpoch::from(epoch).value(), CredentialMode::Bearer, time(0), time(expires) };
}

InstanceSettings settings(std::int64_t maxUsers = 10) {
    return {id<InstanceId>(10), id<OperationId>(11), time(0), UserLimit::from(maxUsers).value(), {ByteCount{}, NodeCount::from(5).value() }, {ByteCount{}, NodeCount::from(5).value() }, Revision{} };
}

std::int64_t scalar(SqliteConnection& c, std::string_view sql) {
    auto q = SqliteStatement::prepare(c, sql);
    BOOST_REQUIRE(q);
    auto row = q.value()->step();
    BOOST_REQUIRE(row && row.value());
    auto n = q.value()->integer(0);
    BOOST_REQUIRE(n);
    return n.value();
}
struct Transaction {
    SqliteTransactionContext context;
    SqliteUserRepository users;
    SqliteSessionRepository sessions;
    SqliteInviteRepository invites;
    SqliteSettingsRepository settings;
    SqliteIdentityAuditRepository audit;
    SqliteAuthorizationReader authorization;
    Transaction(SqliteConnection& c, bool& active, PasswordHashLimits budget = limits)
        : context(c, active), users(context, budget), sessions(context), invites(context), settings(context), audit(context), authorization(context) {}
};
struct Database {
    std::filesystem::path root;
    std::unique_ptr<SqliteConnection> connection;
    bool active = false;
    Database() {
        char pattern[] = "/tmp/anof-sqlite-identity-XXXXXX";
        auto* p = mkdtemp(pattern);
        if (!p) throw std::runtime_error("temporary directory failed");
        root = p;
        BOOST_REQUIRE(createSqliteDatabase(root/"db", {0 }));
        connection = open();
        Transaction t(*connection, active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.users.insert(newUser(1, "admin", UserRole::Administrator)));
        BOOST_REQUIRE(t.users.insert(newUser(2)));
        BOOST_REQUIRE(t.context.commit());
    }
    ~Database() {
        connection.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::unique_ptr<SqliteConnection> open() {auto c = openSqliteDatabase(root/"db", {0 });
        BOOST_REQUIRE(c);
        return std::move(c).value();
    }
};
}

BOOST_AUTO_TEST_CASE(user_reads_are_owned_and_missing_is_optional) {
    Database f;
    Transaction t(*f.connection, f.active);
    BOOST_REQUIRE(t.context.begin());
    auto found = t.users.find(id<UserId>(2));
    BOOST_REQUIRE(found && found.value());
    BOOST_CHECK_EQUAL(found.value()->login.value(), "member");
    auto password = t.users.findPassword(LoginName::parse("MEMBER").value());
    BOOST_REQUIRE(password && password.value());
    auto missing = t.users.find(id<UserId>(99));
    BOOST_REQUIRE(missing);
    BOOST_CHECK(!missing.value());
    auto absent = t.users.findPassword(LoginName::parse("absent").value());
    BOOST_REQUIRE(absent);
    BOOST_CHECK(!absent.value());
    BOOST_CHECK_EQUAL(t.users.countUsers().value().value(), 2);
    BOOST_REQUIRE(t.context.commit());
    BOOST_CHECK(password.value()->encodedHash.view() == hashText);
    errorIs(t.users.countUsers(), ErrorCode::InvalidArgument);
    Transaction next(*f.connection, f.active);
    BOOST_REQUIRE(next.context.begin());
    errorIs(t.users.find(id<UserId>(1)), ErrorCode::InvalidArgument);
    BOOST_REQUIRE(next.context.commit());
}

BOOST_AUTO_TEST_CASE(login_conflict_and_invalid_initial_values_poison_transaction) {
    Database f;
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.users.insert(newUser(3, "another")));
        errorIs(t.users.insert(newUser(4, "MEMBER")), ErrorCode::LoginConflict);
        BOOST_CHECK(!t.context.commit());
    }
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM users"), 2);
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto u = newUser(3);
        u.profile.authEpoch = AuthEpoch::from(1).value();
        errorIs(t.users.insert(u), ErrorCode::InvalidArgument);
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.insert(newUser(2, "different")), ErrorCode::IntegrityError);
    }
}

BOOST_AUTO_TEST_CASE(display_and_status_changes_compare_revision_and_preserve_noops) {
    Database f;
    Transaction t(*f.connection, f.active);
    BOOST_REQUIRE(t.context.begin());
    auto same = t.users.setDisplayName(id<UserId>(2), Revision{}, DisplayName::parse("Member").value(), time(1));
    BOOST_REQUIRE(same);
    BOOST_CHECK_EQUAL(same.value().revision.value(), 1);
    BOOST_CHECK(same.value().updatedAt == time(0));
    BOOST_CHECK_EQUAL(same.value().authEpoch.value(), 0);
    auto changed = t.users.setDisplayName(id<UserId>(2), Revision{}, DisplayName::parse("新名字").value(), time(2));
    BOOST_REQUIRE(changed);
    BOOST_CHECK_EQUAL(changed.value().revision.value(), 2);
    BOOST_CHECK(changed.value().updatedAt == time(2));
    BOOST_CHECK(changed.value().createdAt == time(0));
    BOOST_CHECK_EQUAL(changed.value().authEpoch.value(), 0);
    auto disabled = t.users.setEnabled(id<UserId>(2), Revision::from(2).value(), false, time(3));
    BOOST_REQUIRE(disabled);
    BOOST_CHECK_EQUAL(disabled.value().authEpoch.value(), 1);
    BOOST_CHECK_EQUAL(disabled.value().revision.value(), 3);
    BOOST_CHECK(disabled.value().updatedAt == time(3));
    auto noop = t.users.setEnabled(id<UserId>(2), Revision::from(3).value(), false, time(4));
    BOOST_REQUIRE(noop);
    BOOST_CHECK_EQUAL(noop.value().revision.value(), 3);
    BOOST_CHECK(noop.value().updatedAt == time(3));
    BOOST_CHECK_EQUAL(noop.value().authEpoch.value(), 1);
    auto enabled = t.users.setEnabled(id<UserId>(2), Revision::from(3).value(), true, time(5));
    BOOST_REQUIRE(enabled);
    BOOST_CHECK_EQUAL(enabled.value().authEpoch.value(), 1);
    BOOST_CHECK_EQUAL(enabled.value().revision.value(), 4);
    BOOST_CHECK(enabled.value().updatedAt == time(5));
    BOOST_CHECK(enabled.value().createdAt == time(0));
    BOOST_REQUIRE(t.context.commit());
    {
        Transaction stale(*f.connection, f.active);
        BOOST_REQUIRE(stale.context.begin());
        errorIs(stale.users.setEnabled(id<UserId>(2), Revision{}, true, time(6)), ErrorCode::PreconditionFailed);
    }
    {
        Transaction admin(*f.connection, f.active);
        BOOST_REQUIRE(admin.context.begin());
        errorIs(admin.users.setEnabled(id<UserId>(1), Revision{}, false, time(6)), ErrorCode::Forbidden);
    }
}

BOOST_AUTO_TEST_CASE(account_conditions_precede_noops_and_administrator_protection) {
    Database f;
    for (unsigned scenario = 0; scenario != 3; ++scenario) {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        const auto stale = Revision::from(2).value();
        if (scenario == 0) {
            errorIs(t.users.setDisplayName(id<UserId>(2), stale, DisplayName::parse("Member").value(), time(9)), ErrorCode::PreconditionFailed);
        } else {
            errorIs(t.users.setEnabled(id<UserId>(1), stale, scenario == 1, time(9)), ErrorCode::PreconditionFailed);
        }
        BOOST_CHECK(!t.context.commit());
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto unchanged = t.users.setEnabled(id<UserId>(1), Revision{}, true, time(9));
        BOOST_REQUIRE(unchanged);
        BOOST_CHECK_EQUAL(unchanged.value().revision.value(), 1);
        BOOST_CHECK_EQUAL(unchanged.value().authEpoch.value(), 0);
        BOOST_CHECK(unchanged.value().updatedAt == time(0));
        BOOST_REQUIRE(t.context.commit());
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.setEnabled(id<UserId>(99), Revision{}, false, time(9)), ErrorCode::NotFound);
        BOOST_CHECK(!t.context.commit());
    }
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM users WHERE revision=1 AND auth_epoch=0 AND updated_at=0 AND enabled=1"), 2);
}

BOOST_AUTO_TEST_CASE(password_replacement_checks_each_expectation_and_enabled_state) {
    Database f;
    for (unsigned scenario = 0; scenario != 4; ++scenario) {
        // 独立改变一项预期；禁用场景使用完全匹配的当前版本。
        if (scenario == 3) {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), Revision{}, false, time(1)));
            BOOST_REQUIRE(t.context.commit());
        }
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto before = t.users.findPasswordById(id<UserId>(2));
        BOOST_REQUIRE(before && before.value());
        const auto& value = *before.value();
        PasswordExpectation expected{ value.passwordVersion, value.authEpoch, value.userRevision };
        if (scenario == 0) expected.passwordVersion = expected.passwordVersion.add(1).value();
        if (scenario == 1) expected.authEpoch = expected.authEpoch.add(1).value();
        if (scenario == 2) expected.userRevision = expected.userRevision.add(1).value();
        errorIs(t.users.replacePassword(id<UserId>(2), expected, hash(), time(9)), ErrorCode::PreconditionFailed);
        BOOST_CHECK(!t.context.commit());
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT password_version FROM users WHERE login='member'"), 1);
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT revision FROM users WHERE login='member'"), value.userRevision.value());
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT auth_epoch FROM users WHERE login='member'"), value.authEpoch.value());
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT updated_at FROM users WHERE login='member'"), scenario == 3 ? 1 : 0);
    }
}

BOOST_AUTO_TEST_CASE(password_replacement_persists_hash_and_same_hash_is_still_a_change) {
    Database f;
    std::string replacement(hashText);
    replacement.back() = 'E';
    auto parsed = EncodedPasswordHash::parse(replacement, limits);
    BOOST_REQUIRE(parsed);
    for (std::int64_t round = 1; round <= 2; ++round) {
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            auto changed = t.users.replacePassword(id<UserId>(2),
                { PasswordVersion::from(round).value(), AuthEpoch::from(round - 1).value(), Revision::from(round).value() },
                parsed.value(), time(round));
            BOOST_REQUIRE(changed);
            BOOST_CHECK_EQUAL(changed.value().revision.value(), round + 1);
            BOOST_CHECK_EQUAL(changed.value().authEpoch.value(), round);
            BOOST_CHECK(changed.value().updatedAt == time(round));
            BOOST_CHECK(changed.value().createdAt == time(0));
            BOOST_CHECK(changed.value().enabled && changed.value().role == UserRole::Member);
            BOOST_CHECK_EQUAL(changed.value().login.value(), "member");
            BOOST_CHECK_EQUAL(changed.value().displayName.value(), "Member");
            BOOST_REQUIRE(t.context.commit());
        }
        Transaction read(*f.connection, f.active);
        BOOST_REQUIRE(read.context.beginRead());
        auto stored = read.users.findPasswordById(id<UserId>(2));
        BOOST_REQUIRE(stored && stored.value());
        BOOST_CHECK(stored.value()->encodedHash.view() == replacement);
        BOOST_CHECK_EQUAL(stored.value()->passwordVersion.value(), round + 1);
        BOOST_CHECK_EQUAL(stored.value()->authEpoch.value(), round);
        BOOST_CHECK_EQUAL(stored.value()->userRevision.value(), round + 1);
    }
}

BOOST_AUTO_TEST_CASE(profile_and_status_overflow_preserve_state_but_allow_noops) {
    Database f;
    const auto maximum = Revision::from(std::numeric_limits<std::int64_t>::max()).value();
    BOOST_REQUIRE(f.connection->execute("UPDATE users SET revision=9223372036854775807 WHERE login='member'"));
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.users.setDisplayName(id<UserId>(2), maximum, DisplayName::parse("Member").value(), time(9)));
        BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), maximum, true, time(9)));
        BOOST_REQUIRE(t.context.commit());
    }
    for (unsigned scenario = 0; scenario != 3; ++scenario) {
        if (scenario == 2) BOOST_REQUIRE(f.connection->execute("UPDATE users SET revision=1,auth_epoch=9223372036854775807 WHERE login='member'"));
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        if (scenario == 0) {
            errorIs(t.users.setDisplayName(id<UserId>(2), maximum, DisplayName::parse("Changed").value(), time(9)), ErrorCode::IntegrityError);
        } else {
            errorIs(t.users.setEnabled(id<UserId>(2), scenario == 1 ? maximum : Revision{}, false, time(9)), ErrorCode::IntegrityError);
        }
        BOOST_CHECK(!t.context.commit());
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM users WHERE login='member' AND display_name='Member' AND enabled=1 AND updated_at=0"), 1);
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT revision FROM users WHERE login='member'"), scenario == 2 ? 1 : maximum.value());
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT auth_epoch FROM users WHERE login='member'"), scenario == 2 ? maximum.value() : 0);
    }
    // 启用不递增 epoch；即使 epoch 已达上限仍可启用。
    BOOST_REQUIRE(f.connection->execute("UPDATE users SET enabled=0 WHERE login='member'"));
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto enabled = t.users.setEnabled(id<UserId>(2), Revision{}, true, time(10));
        BOOST_REQUIRE(enabled);
        BOOST_CHECK_EQUAL(enabled.value().authEpoch.value(), maximum.value());
        BOOST_CHECK_EQUAL(enabled.value().revision.value(), 2);
        BOOST_REQUIRE(t.context.commit());
    }
}

BOOST_AUTO_TEST_CASE(display_update_and_caller_noops_do_not_revoke_or_duplicate_audit) {
    Database f;
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.sessions.insert(newSession(1)));
        BOOST_REQUIRE(t.context.commit());
    }
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.authorization.requireActiveSession({ digest(1), CredentialMode::Bearer }, time(5)));
        auto before = t.users.find(id<UserId>(2));
        BOOST_REQUIRE(before && before.value());
        auto changed = t.users.setDisplayName(id<UserId>(2), before.value()->revision, DisplayName::parse("Changed").value(), time(5 + attempt));
        BOOST_REQUIRE(changed);
        // 调用方根据版本是否改变决定审计，仓储不自行追加事件。
        if (changed.value().revision != before.value()->revision)
            BOOST_REQUIRE(t.audit.append({ id<RequestId>(20), time(5), DisplayNameChanged{ id<UserId>(2) } }));
        auto enabled = t.users.setEnabled(id<UserId>(2), changed.value().revision, true, time(9));
        BOOST_REQUIRE(enabled);
        if (enabled.value().revision != changed.value().revision)
            BOOST_REQUIRE(t.audit.append({ id<RequestId>(21), time(9), UserEnabled{ id<UserId>(1), id<UserId>(2) } }));
        BOOST_REQUIRE(t.context.commit());
    }
    auto observer = f.open();
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM identity_audit_events WHERE action='display_name_changed'"), 1);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM identity_audit_events"), 1);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM sessions WHERE revoked=0"), 1);
    Transaction t(*observer, f.active);
    BOOST_REQUIRE(t.context.beginRead());
    BOOST_REQUIRE(t.authorization.requireActiveSession({ digest(1), CredentialMode::Bearer }, time(10)));
    auto profile = t.users.find(id<UserId>(2));
    BOOST_REQUIRE(profile && profile.value());
    BOOST_CHECK_EQUAL(profile.value()->revision.value(), 2);
    BOOST_CHECK_EQUAL(profile.value()->authEpoch.value(), 0);
    BOOST_CHECK(profile.value()->updatedAt == time(5));
}

BOOST_AUTO_TEST_CASE(disable_revoke_and_reenable_commit_without_reviving_old_sessions) {
    Database f;
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.sessions.insert(newSession(1)));
        BOOST_REQUIRE(t.sessions.insert(newSession(2)));
        BOOST_REQUIRE(t.sessions.insert(newSession(3, 1)));
        BOOST_REQUIRE(t.context.commit());
    }
    auto observer = f.open();
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.authorization.requireActiveSession({ digest(3), CredentialMode::Bearer }, time(5)));
        BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), Revision{}, false, time(5)));
        auto revoked = t.sessions.revokeAll(id<UserId>(2), time(5));
        BOOST_REQUIRE(revoked);
        BOOST_CHECK_EQUAL(revoked.value().value(), 2);
        BOOST_REQUIRE(t.audit.append({ id<RequestId>(20), time(5), UserDisabled{ id<UserId>(1), id<UserId>(2) } }));
        BOOST_CHECK_EQUAL(scalar(*observer, "SELECT enabled FROM users WHERE login='member'"), 1);
        BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM sessions WHERE revoked=1"), 0);
        BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM identity_audit_events"), 0);
        BOOST_REQUIRE(t.context.commit());
    }
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT enabled FROM users WHERE login='member'"), 0);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM sessions WHERE revoked=1"), 2);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM identity_audit_events WHERE action='user_disabled'"), 1);
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), Revision::from(2).value(), true, time(6)));
        BOOST_REQUIRE(t.audit.append({ id<RequestId>(21), time(6), UserEnabled{ id<UserId>(1), id<UserId>(2) } }));
        BOOST_REQUIRE(t.context.commit());
    }
    for (unsigned n : { 1, 2 }) {
        Transaction t(*observer, f.active);
        BOOST_REQUIRE(t.context.beginRead());
        errorIs(t.authorization.requireActiveSession({ digest(n), CredentialMode::Bearer }, time(7)), ErrorCode::Unauthenticated);
    }
    Transaction t(*f.connection, f.active);
    BOOST_REQUIRE(t.context.begin());
    BOOST_REQUIRE(t.authorization.requireActiveSession({ digest(3), CredentialMode::Bearer }, time(7)));
    BOOST_REQUIRE(t.sessions.insert(newSession(4, 2, 1)));
    BOOST_REQUIRE(t.authorization.requireActiveSession({ digest(4), CredentialMode::Bearer }, time(7)));
    BOOST_REQUIRE(t.context.commit());
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT revision FROM users WHERE login='member'"), 3);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT auth_epoch FROM users WHERE login='member'"), 1);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM identity_audit_events"), 2);
}

BOOST_AUTO_TEST_CASE(account_audit_failure_or_abandonment_rolls_back_updates_and_revocations) {
    for (unsigned operation = 0; operation != 4; ++operation) {
        for (unsigned failure = 0; failure != 3; ++failure) {
            Database f;
            {
                Transaction seed(*f.connection, f.active);
                BOOST_REQUIRE(seed.context.begin());
                BOOST_REQUIRE(seed.sessions.insert(newSession(1)));
                BOOST_REQUIRE(seed.sessions.insert(newSession(2)));
                if (operation == 3) BOOST_REQUIRE(seed.users.setEnabled(id<UserId>(2), Revision{}, false, time(1)));
                BOOST_REQUIRE(seed.context.commit());
            }
            // 先通过当前 schema 校验打开观察连接，再安装测试专用触发器。
            auto observer = f.open();
            if (failure != 2) BOOST_REQUIRE(f.connection->execute(failure == 0
                ? "CREATE TRIGGER reject_account_audit BEFORE INSERT ON identity_audit_events BEGIN SELECT RAISE(ABORT,'injected'); END"
                : "CREATE TRIGGER ignore_account_audit BEFORE INSERT ON identity_audit_events BEGIN SELECT RAISE(IGNORE); END"));
            {
                Transaction t(*f.connection, f.active);
                BOOST_REQUIRE(t.context.begin());
                IdentityAction event = DisplayNameChanged{ id<UserId>(2) };
                if (operation == 0) {
                    BOOST_REQUIRE(t.users.setDisplayName(id<UserId>(2), Revision{}, DisplayName::parse("Changed").value(), time(5)));
                } else if (operation == 1) {
                    BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), Revision{}, false, time(5)));
                    event = UserDisabled{ id<UserId>(1), id<UserId>(2) };
                } else if (operation == 2) {
                    std::string replacement(hashText);
                    replacement.back() = 'E';
                    auto encoded = EncodedPasswordHash::parse(replacement, limits);
                    BOOST_REQUIRE(encoded);
                    BOOST_REQUIRE(t.users.replacePassword(id<UserId>(2), { PasswordVersion{}, AuthEpoch{}, Revision{} }, encoded.value(), time(5)));
                    event = PasswordChanged{ id<UserId>(2) };
                } else {
                    BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), Revision::from(2).value(), true, time(5)));
                    event = UserEnabled{ id<UserId>(1), id<UserId>(2) };
                }
                if (operation == 1 || operation == 2) {
                    auto revoked = t.sessions.revokeAll(id<UserId>(2), time(5));
                    BOOST_REQUIRE(revoked);
                    BOOST_CHECK_EQUAL(revoked.value().value(), 2);
                }
                auto audited = t.audit.append({ id<RequestId>(20), time(5), event });
                if (failure == 2) BOOST_REQUIRE(audited); // 析构回滚包含已写入的审计。
                else {
                    errorIs(audited, ErrorCode::IntegrityError);
                    BOOST_CHECK(!t.context.commit());
                }
            }
            BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM identity_audit_events"), 0);
            BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM sessions WHERE revoked=0"), 2);
            BOOST_CHECK_EQUAL(scalar(*observer, "SELECT password_version FROM users WHERE login='member'"), 1);
            BOOST_CHECK_EQUAL(scalar(*observer, std::string("SELECT count(*) FROM users WHERE login='member' AND password_hash='") + hashText + "'"), 1);
            BOOST_CHECK_EQUAL(scalar(*observer, "SELECT revision FROM users WHERE login='member'"), operation == 3 ? 2 : 1);
            BOOST_CHECK_EQUAL(scalar(*observer, "SELECT auth_epoch FROM users WHERE login='member'"), operation == 3 ? 1 : 0);
            BOOST_CHECK_EQUAL(scalar(*observer, "SELECT enabled FROM users WHERE login='member'"), operation == 3 ? 0 : 1);
            BOOST_CHECK_EQUAL(scalar(*observer, "SELECT updated_at FROM users WHERE login='member'"), operation == 3 ? 1 : 0);
            BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM users WHERE login='member' AND display_name='Member'"), 1);
        }
    }
}

BOOST_AUTO_TEST_CASE(password_update_commit_unknown_closes_transaction_and_quarantines_connection) {
    Database f;
    {
        Transaction seed(*f.connection, f.active);
        BOOST_REQUIRE(seed.context.begin());
        BOOST_REQUIRE(seed.sessions.insert(newSession(1)));
        BOOST_REQUIRE(seed.context.commit());
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.authorization.requireActiveSession({ digest(1), CredentialMode::Bearer }, time(5)));
        BOOST_REQUIRE(t.users.replacePassword(id<UserId>(2), { PasswordVersion{}, AuthEpoch{}, Revision{} }, hash(), time(5)));
        BOOST_REQUIRE(t.sessions.revokeAll(id<UserId>(2), time(5)));
        BOOST_REQUIRE(t.audit.append({ id<RequestId>(20), time(5), PasswordChanged{ id<UserId>(2) } }));
        auto deny = [](void*, int operation, const char* first, const char*, const char*, const char*) {
            return operation == SQLITE_TRANSACTION && first && std::string_view(first) != "BEGIN" ? SQLITE_DENY : SQLITE_OK;
        };
        BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(f.connection->handle(), deny, nullptr), SQLITE_OK);
        auto result = t.context.commit();
        BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(f.connection->handle(), nullptr, nullptr), SQLITE_OK);
        errorIs(result, ErrorCode::CommitOutcomeUnknown);
        BOOST_CHECK(!f.active && !f.connection->usable());
        errorIs(t.context.commit(), ErrorCode::InvalidArgument);
        errorIs(t.users.find(id<UserId>(2)), ErrorCode::InvalidArgument);
        errorIs(t.sessions.revokeAll(id<UserId>(2), time(6)), ErrorCode::InvalidArgument);
        errorIs(t.audit.append({ id<RequestId>(21), time(6), PasswordChanged{ id<UserId>(2) } }), ErrorCode::InvalidArgument);
    }
    // 这里只验证拒绝 COMMIT/ROLLBACK 的确定性注入；关闭后重开观察，不自动重试业务。
    f.connection.reset();
    auto observer = f.open();
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT revision FROM users WHERE login='member'"), 1);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT password_version FROM users WHERE login='member'"), 1);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT auth_epoch FROM users WHERE login='member'"), 0);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM sessions WHERE revoked=0"), 1);
    BOOST_CHECK_EQUAL(scalar(*observer, "SELECT count(*) FROM identity_audit_events"), 0);
}

BOOST_AUTO_TEST_CASE(password_change_revocation_and_audit_share_commit) {
    Database f;
    Transaction t(*f.connection, f.active);
    BOOST_REQUIRE(t.context.begin());
    BOOST_REQUIRE(t.sessions.insert(newSession(1)));
    BOOST_REQUIRE(t.sessions.insert(newSession(2)));
    BOOST_REQUIRE(t.authorization.requireActiveSession({ digest(1), CredentialMode::Bearer }, time(5)));
    auto changed = t.users.replacePassword(id<UserId>(2), {PasswordVersion{}, AuthEpoch{}, Revision{} }, hash(), time(5));
    BOOST_REQUIRE(changed);
    BOOST_CHECK_EQUAL(changed.value().authEpoch.value(), 1);
    BOOST_CHECK_EQUAL(changed.value().revision.value(), 2);
    BOOST_CHECK_EQUAL(t.sessions.countActive(id<UserId>(2), time(5)).value().value(), 0);
    BOOST_CHECK_EQUAL(t.sessions.revokeAll(id<UserId>(2), time(5)).value().value(), 2);
    BOOST_CHECK_EQUAL(t.sessions.revokeAll(id<UserId>(2), time(5)).value().value(), 0);
    BOOST_REQUIRE(t.audit.append({id<RequestId>(20), time(5), PasswordChanged{ id<UserId>(2) } }));
    BOOST_REQUIRE(t.context.commit());
    Transaction stale(*f.connection, f.active);
    BOOST_REQUIRE(stale.context.begin());
    errorIs(stale.users.replacePassword(id<UserId>(2), {PasswordVersion{}, AuthEpoch{}, Revision{} }, hash(), time(6)), ErrorCode::PreconditionFailed);
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT password_version FROM users WHERE login='member'"), 2);
}

BOOST_AUTO_TEST_CASE(revision_and_epoch_overflow_do_not_partially_write) {
    Database f;
    for (const char* column : {"revision", "auth_epoch", "password_version"}) {
        BOOST_REQUIRE(f.connection->execute(std::string("UPDATE users SET revision=1,auth_epoch=0,password_version=1,") + column+"=9223372036854775807 WHERE login='member'"));
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            auto snapshot = t.users.findPasswordById(id<UserId>(2));
            BOOST_REQUIRE(snapshot && snapshot.value());
            const auto& s = *snapshot.value();
            errorIs(t.users.replacePassword(id<UserId>(2), {s.passwordVersion, s.authEpoch, s.userRevision }, hash(), time(1)), ErrorCode::IntegrityError);
        }
        BOOST_CHECK_EQUAL(scalar(*f.connection, std::string("SELECT ") + column+" FROM users WHERE login='member'"), std::numeric_limits<std::int64_t>::max());
    }
}

BOOST_AUTO_TEST_CASE(session_authorization_modes_expiry_epoch_and_revocation) {
    Database f;
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.sessions.insert(newSession(1)));
        BOOST_REQUIRE(t.context.commit());
    }
    for (int scenario = 0;scenario != 6;++scenario) {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        SessionCredential credential{ digest(1), CredentialMode::Bearer };
        UtcTime now = time(99);
        if (scenario == 1) credential.digest = digest(2);
        if (scenario == 2) now = time(100);
        if (scenario == 3) credential.mode = static_cast<CredentialMode>(99);
        if (scenario == 4) {
            auto revoked = t.sessions.revoke(credential, id<UserId>(2), time(1));
            BOOST_REQUIRE(revoked && revoked.value());
            auto twice = t.sessions.revoke(credential, id<UserId>(2), time(1));
            BOOST_REQUIRE(twice && !twice.value());
        }
        if (scenario == 5) BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), Revision{}, false, time(1)));
        auto result = t.authorization.requireActiveSession(credential, now);
        if (!scenario) BOOST_REQUIRE(result);
        else errorIs(result, ErrorCode::Unauthenticated);
        // 不提交；各失效情形不影响其他情形的夹具。
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), Revision{}, false, time(1)));
        BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), Revision::from(2).value(), true, time(2)));
        errorIs(t.authorization.requireActiveSession({digest(1), CredentialMode::Bearer }, time(3)), ErrorCode::Unauthenticated);
    }
}

BOOST_AUTO_TEST_CASE(session_cleanup_is_bounded_ordered_and_does_not_remove_active_rows) {
    Database f;
    Transaction t(*f.connection, f.active);
    BOOST_REQUIRE(t.context.begin());
    for (unsigned n = 1;n <= 5;++n) BOOST_REQUIRE(t.sessions.insert(newSession(n, 2, 0, n == 5 ? 100:10)));
    BOOST_CHECK_EQUAL(t.sessions.countActive(id<UserId>(2), time(10)).value().value(), 1);
    auto erased = t.sessions.eraseInactive(time(10), PageSize::from(2).value());
    BOOST_REQUIRE(erased);
    BOOST_CHECK_EQUAL(erased.value().value(), 2);
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM sessions WHERE token_hash IN (x'0100000000000000000000000000000000000000000000000000000000000000',x'0200000000000000000000000000000000000000000000000000000000000000')"), 0);
    BOOST_CHECK_EQUAL(t.sessions.eraseInactive(time(10), PageSize::from(200).value()).value().value(), 2);
    BOOST_CHECK_EQUAL(t.sessions.eraseInactive(time(10), PageSize{}).value().value(), 0);
    BOOST_REQUIRE(t.context.commit());
}

BOOST_AUTO_TEST_CASE(invite_consume_rollback_unavailability_and_seek_pagination) {
    Database f;
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        for(unsigned n = 3;n < 7;++n) BOOST_REQUIRE(t.invites.insert(newInvite(n)));
        BOOST_REQUIRE(t.context.commit());
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.invites.consume(inviteDigest(3), time(1)));
        errorIs(t.users.insert(newUser(8, "member")), ErrorCode::LoginConflict);
        BOOST_CHECK(!t.context.commit());
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto found = t.invites.find(id<InviteId>(3));
        BOOST_REQUIRE(found && found.value());
        BOOST_CHECK_EQUAL(found.value()->usedCount.value(), 0);
        auto page = t.invites.list(PageSize::from(2).value(), {});
        BOOST_REQUIRE(page && page.value().next);
        BOOST_CHECK(page.value().items[0].id == id<InviteId>(3));
        auto tail = t.invites.list(PageSize::from(2).value(), page.value().next);
        BOOST_REQUIRE(tail);
        BOOST_CHECK_EQUAL(tail.value().items.size(), 2);
        BOOST_CHECK(!tail.value().next);
        BOOST_REQUIRE(t.invites.consume(inviteDigest(3), time(1)));
        BOOST_REQUIRE(t.context.commit());
    }
    for(unsigned n : {3U, 4U, 99U }) {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.invites.consume(inviteDigest(n), time(n == 4 ? 100:1)), ErrorCode::InviteUnavailable);
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto first = t.invites.revoke(id<InviteId>(4), time(1));
        BOOST_REQUIRE(first && first.value());
        auto second = t.invites.revoke(id<InviteId>(4), time(2));
        BOOST_REQUIRE(second && !second.value());
        errorIs(t.invites.consume(inviteDigest(4), time(3)), ErrorCode::InviteUnavailable);
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.invites.revoke(id<InviteId>(99), time(1)), ErrorCode::NotFound);
    }
}

BOOST_AUTO_TEST_CASE(settings_optional_insert_duplicate_and_invalid_initial_values) {
    Database f;
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto empty = t.settings.get();
        BOOST_REQUIRE(empty && !empty.value());
        BOOST_REQUIRE(t.settings.insertBootstrap(settings()));
        BOOST_REQUIRE(t.audit.append({id<RequestId>(20), time(0), InstanceInitialized{ id<UserId>(1), id<InstanceId>(10) } }));
        BOOST_REQUIRE(t.context.commit());
    }
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto got = t.settings.get();
        BOOST_REQUIRE(got && got.value());
        BOOST_CHECK(got.value()->instanceId == id<InstanceId>(10));
        errorIs(t.settings.insertBootstrap(settings()), ErrorCode::Conflict);
    }
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM identity_audit_events"), 1);
}

BOOST_AUTO_TEST_CASE(identity_audit_encodes_every_variant_without_secret_columns) {
    Database f;
    Transaction t(*f.connection, f.active);
    BOOST_REQUIRE(t.context.begin());
    BOOST_REQUIRE(t.settings.insertBootstrap(settings()));
    BOOST_REQUIRE(t.invites.insert(newInvite(3)));
    auto admin = id<UserId>(1);
    auto member = id<UserId>(2);
    auto invite = id<InviteId>(3);
    auto space = id<SpaceId>(4);
    auto root = id<NodeId>(5);
    BOOST_REQUIRE(f.connection->execute("INSERT INTO spaces VALUES('"+space.toString()+"',1,NULL,'"+root.toString()+"'); INSERT INTO nodes VALUES('"+root.toString()+"','"+space.toString()+"',NULL,'',0,'"+admin.toString()+"',NULL,1,0,0,0)"));
    const IdentityAction actions[] = {InstanceInitialized{ admin, id<InstanceId>(10) }, InviteCreated{ admin, invite }, InviteRevoked{ admin, invite }, UserRegistered{ member, invite }, SessionCreated{ member }, SessionRevoked{ member }, PasswordChanged{ member }, UserEnabled{ admin, member }, UserDisabled{ admin, member }, DisplayNameChanged{ member }, QuotaChanged{ admin, SpaceQuota{ space } }, QuotaChanged{ admin, PublicContributionQuota{ space, member } } };
    for(const auto& action:actions) BOOST_REQUIRE(t.audit.append({id<RequestId>(20), time(0), action }));
    BOOST_REQUIRE(t.context.commit());
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM identity_audit_events"), 12);
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM identity_audit_events WHERE action='session_created' AND actor_id=subject_user_id"), 1);
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM pragma_table_info('identity_audit_events') WHERE name IN ('password_hash','token_hash','token','password')"), 0);
}

BOOST_AUTO_TEST_CASE(audit_failure_rolls_back_user_session_and_invite_changes) {
    Database f;
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.invites.insert(newInvite(3)));
        BOOST_REQUIRE(t.context.commit());
    }
    BOOST_REQUIRE(f.connection->execute("CREATE TRIGGER reject_audit BEFORE INSERT ON identity_audit_events BEGIN SELECT RAISE(ABORT,'injected'); END"));
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.users.insert(newUser(4, "new_user")));
        BOOST_REQUIRE(t.invites.consume(inviteDigest(3), time(1)));
        BOOST_REQUIRE(t.sessions.insert(newSession(1, 4)));
        errorIs(t.audit.append({id<RequestId>(20), time(1), UserRegistered{ id<UserId>(4), id<InviteId>(3) } }), ErrorCode::IntegrityError);
        BOOST_CHECK(!t.context.commit());
    }
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM users"), 2);
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM sessions"), 0);
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT used_count FROM invites"), 0);
}

BOOST_AUTO_TEST_CASE(independent_connections_serialize_login_conflict_and_last_invite) {
    Database f;
    auto other = f.open();
    bool otherActive = false;
    {
        Transaction a(*f.connection, f.active);
        Transaction b(*other, otherActive);
        BOOST_REQUIRE(a.context.begin());
        BOOST_REQUIRE(a.users.insert(newUser(3, "contended")));
        errorIs(b.context.begin(), ErrorCode::Retryable);
        BOOST_REQUIRE(a.context.commit());
    }
    {
        Transaction b(*other, otherActive);
        BOOST_REQUIRE(b.context.begin());
        errorIs(b.users.insert(newUser(4, "contended")), ErrorCode::LoginConflict);
    }
    {
        Transaction a(*f.connection, f.active);
        BOOST_REQUIRE(a.context.begin());
        BOOST_REQUIRE(a.invites.insert(newInvite(5)));
        BOOST_REQUIRE(a.context.commit());
    }
    {
        Transaction a(*f.connection, f.active);
        Transaction b(*other, otherActive);
        BOOST_REQUIRE(a.context.begin());
        BOOST_REQUIRE(a.invites.consume(inviteDigest(5), time(1)));
        errorIs(b.context.begin(), ErrorCode::Retryable);
        BOOST_REQUIRE(a.context.commit());
    }
    {
        Transaction b(*other, otherActive);
        BOOST_REQUIRE(b.context.begin());
        errorIs(b.invites.consume(inviteDigest(5), time(1)), ErrorCode::InviteUnavailable);
    }
    BOOST_CHECK_EQUAL(scalar(*other, "SELECT used_count FROM invites"), 1);
}

BOOST_AUTO_TEST_CASE(caller_count_and_insert_limits_are_serialized_by_same_write_transaction) {
    Database f;
    auto other = f.open();
    bool otherActive = false;
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.settings.insertBootstrap(settings(3)));
        for(unsigned n = 1;n <= 31;++n) BOOST_REQUIRE(t.sessions.insert(newSession(n)));
        BOOST_REQUIRE(t.context.commit());
    }
    {
        Transaction a(*f.connection, f.active);
        Transaction b(*other, otherActive);
        BOOST_REQUIRE(a.context.begin());
        BOOST_CHECK_EQUAL(a.users.countUsers().value().value(), 2);
        BOOST_REQUIRE(a.users.insert(newUser(3, "last_user")));
        BOOST_CHECK_EQUAL(a.sessions.countActive(id<UserId>(2), time(1)).value().value(), 31);
        BOOST_REQUIRE(a.sessions.insert(newSession(32)));
        errorIs(b.context.begin(), ErrorCode::Retryable);
        BOOST_REQUIRE(a.context.commit());
    }
    {
        Transaction b(*other, otherActive);
        BOOST_REQUIRE(b.context.begin());
        auto policy = b.settings.get();
        BOOST_REQUIRE(policy && policy.value());
        BOOST_CHECK(b.users.countUsers().value().value() >= policy.value()->maxUsers.value());
        BOOST_CHECK(b.sessions.countActive(id<UserId>(2), time(1)).value().value() >= kMaxActiveSessions);
        BOOST_REQUIRE(b.context.commit());
    }
    // 这里是调用方的检查流程；仓储不静默踢旧 Session 或自行执行注册编排。
    BOOST_CHECK_EQUAL(scalar(*other, "SELECT count(*) FROM users"), 3);
    BOOST_CHECK_EQUAL(scalar(*other, "SELECT count(*) FROM sessions"), 32);
}

BOOST_AUTO_TEST_CASE(corrupt_persistent_values_and_hash_budget_fail_closed) {
    Database f;
    {
        Transaction t(*f.connection, f.active, {4096, 1, 1 });
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.findPasswordById(id<UserId>(2)), ErrorCode::IntegrityError);
    }
    {
        Transaction t(*f.connection, f.active, {});
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.findPasswordById(id<UserId>(2)), ErrorCode::InvalidArgument);
    }
    BOOST_REQUIRE(f.connection->execute("PRAGMA ignore_check_constraints=ON; UPDATE users SET display_name=CAST(x'ff' AS TEXT) WHERE login='member'; PRAGMA ignore_check_constraints=OFF"));
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.find(id<UserId>(2)), ErrorCode::IntegrityError);
    }
    BOOST_REQUIRE(f.connection->execute("PRAGMA ignore_check_constraints=ON; UPDATE users SET password_hash=password_hash || 'A' WHERE login='member'; PRAGMA ignore_check_constraints=OFF"));
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.findPasswordById(id<UserId>(2)), ErrorCode::IntegrityError);
    }
}

BOOST_AUTO_TEST_CASE(zero_row_trigger_is_not_success_and_finished_context_cannot_reuse) {
    Database f;
    BOOST_REQUIRE(f.connection->execute("CREATE TRIGGER ignore_user BEFORE INSERT ON users BEGIN SELECT RAISE(IGNORE); END"));
    {
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.insert(newUser(3, "ignored")), ErrorCode::IntegrityError);
        BOOST_CHECK(!t.context.commit());
        errorIs(t.sessions.countActive(id<UserId>(2), time(1)), ErrorCode::InvalidArgument);
    }
    BOOST_CHECK(!f.active);
    BOOST_CHECK(!f.connection->inTransaction());
}

BOOST_AUTO_TEST_CASE(commit_failure_does_not_claim_audit_or_identity_success) {
    Database f;
    Transaction t(*f.connection, f.active);
    BOOST_REQUIRE(t.context.begin());
    BOOST_REQUIRE(t.users.insert(newUser(3, "uncertain")));
    BOOST_REQUIRE(t.audit.append({id<RequestId>(20), time(1), SessionCreated{ id<UserId>(3) } }));
    auto deny = [](void*, int operation, const char* first, const char*, const char*, const char*) {
        return operation == SQLITE_TRANSACTION && first && std::string_view(first) != "BEGIN" ? SQLITE_DENY : SQLITE_OK;
    };
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(f.connection->handle(), deny, nullptr), SQLITE_OK);
    errorIs(t.context.commit(), ErrorCode::CommitOutcomeUnknown);
    BOOST_REQUIRE_EQUAL(sqlite3_set_authorizer(f.connection->handle(), nullptr, nullptr), SQLITE_OK);
    BOOST_REQUIRE(f.connection->rollback());
    BOOST_CHECK(!f.connection->usable());
}

BOOST_AUTO_TEST_CASE(invalid_inputs_and_missing_updates_do_not_report_success) {
    Database f;
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto s = newSession(1);
        s.mode = static_cast<CredentialMode>(9);
        errorIs(t.sessions.insert(s), ErrorCode::InvalidArgument);
    }
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.sessions.insert(newSession(1, 2, 1)), ErrorCode::PreconditionFailed);
    }
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto i = newInvite(3);
        i.info.usedCount = InviteUseCount::from(1).value();
        errorIs(t.invites.insert(i), ErrorCode::InvalidArgument);
    }
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto i = newInvite(3, 1, 0, 2592000000001LL);
        errorIs(t.invites.insert(i), ErrorCode::InvalidArgument);
    }
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        auto s = settings();
        s.defaultPrivate.nodes = NodeCount{};
        errorIs(t.settings.insertBootstrap(s), ErrorCode::InvalidArgument);
    }
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.setDisplayName(id<UserId>(99), Revision{}, DisplayName::parse("Name").value(), time(1)), ErrorCode::NotFound);
    }
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.replacePassword(id<UserId>(99), {PasswordVersion{}, AuthEpoch{}, Revision{} }, hash(), time(1)), ErrorCode::NotFound);
    }
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM sessions"), 0);
    BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT count(*) FROM invites"), 0);
}

BOOST_AUTO_TEST_CASE(session_revoke_binds_owner_and_cleanup_includes_revoked_and_stale_epochs) {
    Database f;
    Transaction t(*f.connection, f.active);
    BOOST_REQUIRE(t.context.begin());
    BOOST_REQUIRE(t.sessions.insert(newSession(1)));
    BOOST_REQUIRE(t.sessions.insert(newSession(2)));
    auto wrongOwner = t.sessions.revoke({digest(1), CredentialMode::Bearer }, id<UserId>(1), time(1));
    BOOST_REQUIRE(wrongOwner && !wrongOwner.value());
    BOOST_REQUIRE(t.sessions.revoke({digest(1), CredentialMode::Bearer }, id<UserId>(2), time(1)));
    BOOST_CHECK_EQUAL(t.sessions.eraseInactive(time(1), PageSize{}).value().value(), 1);
    BOOST_REQUIRE(t.users.setEnabled(id<UserId>(2), Revision{}, false, time(2)));
    BOOST_CHECK_EQUAL(t.sessions.eraseInactive(time(2), PageSize::from(200).value()).value().value(), 1);
    BOOST_REQUIRE(t.context.commit());
}

BOOST_AUTO_TEST_CASE(malformed_session_and_oversized_hash_are_integrity_errors) {
    Database f;
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        BOOST_REQUIRE(t.sessions.insert(newSession(1)));
        BOOST_REQUIRE(t.context.commit());
    }
    BOOST_REQUIRE(f.connection->execute("PRAGMA ignore_check_constraints=ON; UPDATE sessions SET credential_mode='cookie'; PRAGMA ignore_check_constraints=OFF"));
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.authorization.requireActiveSession({digest(1), CredentialMode::Bearer }, time(1)), ErrorCode::IntegrityError);
    }
    BOOST_REQUIRE(f.connection->execute("PRAGMA ignore_check_constraints=ON; UPDATE users SET password_hash=CAST(zeroblob(2048) AS TEXT) WHERE login='member'; PRAGMA ignore_check_constraints=OFF"));
    { Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.users.findPasswordById(id<UserId>(2)), ErrorCode::IntegrityError);
    }
}
BOOST_AUTO_TEST_CASE(account_event_times_survive_rollback_and_connection_reopen) {
    for (int operation = 0; operation < 3; ++operation) {
        Database f;
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            auto u = newUser(3, "clock");
            u.profile.createdAt = u.profile.updatedAt = time(100);
            BOOST_REQUIRE(t.users.insert(u));
            BOOST_REQUIRE(t.context.commit());
        }
        auto mutate = [&](Transaction& t, UtcTime observed, bool stale = false) -> Result<UserProfile> {
            auto old = t.users.find(id<UserId>(3));
            BOOST_REQUIRE(old && old.value());
            auto revision = stale ? Revision{} : old.value()->revision;
            if (operation == 0) return t.users.setDisplayName(id<UserId>(3), revision,
                                                              DisplayName::parse("Name" + std::to_string(revision.value())).value(), observed);
            if (operation == 1) return t.users.setEnabled(id<UserId>(3), revision, !old.value()->enabled, observed);
            auto password = t.users.findPasswordById(id<UserId>(3));
            BOOST_REQUIRE(password && password.value());
            const auto& p = *password.value();
            return t.users.replacePassword(id<UserId>(3), { p.passwordVersion, p.authEpoch, revision }, hash(), observed);
        };
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            BOOST_REQUIRE(mutate(t, time(200)));
            // 向前更新时间也随未提交事务回滚。
        }
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT updated_at FROM users WHERE login='clock'"), 100);
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT revision FROM users WHERE login='clock'"), 1);
        if (operation < 2) {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            auto old = t.users.find(id<UserId>(3)).value().value();
            auto unchanged = operation == 0
                                 ? t.users.setDisplayName(old.id, old.revision, old.displayName, time(200))
                                 : t.users.setEnabled(old.id, old.revision, old.enabled, time(200));
            BOOST_REQUIRE(unchanged);
            BOOST_CHECK(unchanged.value().updatedAt == old.updatedAt);
            BOOST_CHECK(unchanged.value().revision == old.revision);
            BOOST_REQUIRE(t.context.commit());
        }
        std::int64_t expectedTime = 100;
        std::int64_t revision = 1;
        for (auto observed : { INT64_MIN, std::int64_t{ 100 }, std::int64_t{ 110 }, std::int64_t{ 105 }, std::int64_t{ 110 }, INT64_MAX }) {
            expectedTime = std::max(expectedTime, observed);
            {
                Transaction t(*f.connection, f.active);
                BOOST_REQUIRE(t.context.begin());
                auto updated = mutate(t, time(observed));
                BOOST_REQUIRE(updated);
                BOOST_CHECK_EQUAL(updated.value().updatedAt.time_since_epoch().count(), expectedTime);
                BOOST_CHECK_EQUAL(updated.value().revision.value(), ++revision);
                auto read = t.users.find(id<UserId>(3));
                BOOST_REQUIRE(read && read.value());
                BOOST_CHECK(read.value()->updatedAt == updated.value().updatedAt);
                BOOST_REQUIRE(t.context.commit());
            }
            f.connection.reset();
            f.connection = f.open();
            BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT updated_at FROM users WHERE login='clock'"), expectedTime);
        }
        // 陈旧条件和未提交变更不得留下 revision／时间变化。
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            errorIs(mutate(t, time(INT64_MIN), true), ErrorCode::PreconditionFailed);
            BOOST_CHECK(!t.context.commit());
        }
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            BOOST_REQUIRE(mutate(t, time(INT64_MIN)));
        }
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT revision FROM users WHERE login='clock'"), revision);
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT updated_at FROM users WHERE login='clock'"), INT64_MAX);
        if (operation < 2) {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            auto old = t.users.find(id<UserId>(3)).value().value();
            auto unchanged = operation == 0
                                 ? t.users.setDisplayName(old.id, old.revision, old.displayName, time(INT64_MIN))
                                 : t.users.setEnabled(old.id, old.revision, old.enabled, time(INT64_MIN));
            BOOST_REQUIRE(unchanged);
            BOOST_CHECK(unchanged.value().updatedAt == old.updatedAt);
            BOOST_CHECK(unchanged.value().revision == old.revision);
            BOOST_CHECK(unchanged.value().authEpoch == old.authEpoch);
            BOOST_REQUIRE(t.context.commit());
        }
    }
}
BOOST_AUTO_TEST_CASE(invite_revoke_event_time_is_persistent_and_expiry_uses_raw_utc) {
    for (auto observed : { std::int64_t{ 99 }, std::int64_t{ 100 }, std::int64_t{ 150 } }) {
        Database f;
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            BOOST_REQUIRE(t.invites.insert(newInvite(1, 5, 100, 200)));
            BOOST_REQUIRE(t.context.commit());
        }
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            // 未提交的撤销不可留下时间。
            BOOST_REQUIRE(t.invites.revoke(id<InviteId>(1), time(observed)).value());
        }
        BOOST_CHECK_EQUAL(scalar(*f.connection, "SELECT revoked_at IS NULL FROM invites"), 1);
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            errorIs(t.invites.consume(inviteDigest(1), time(200)), ErrorCode::InviteUnavailable);
        }
        {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            BOOST_REQUIRE(t.invites.consume(inviteDigest(1), time(99)));
            BOOST_REQUIRE(t.invites.revoke(id<InviteId>(1), time(observed)).value());
            BOOST_REQUIRE(t.context.commit());
        }
        f.connection.reset();
        f.connection = f.open();
        const auto expected = std::max(observed, std::int64_t{ 100 });
        for (auto repeated : { std::int64_t{ 0 }, expected, std::int64_t{ 300 } }) {
            Transaction t(*f.connection, f.active);
            BOOST_REQUIRE(t.context.begin());
            auto revoked = t.invites.revoke(id<InviteId>(1), time(repeated));
            BOOST_REQUIRE(revoked);
            BOOST_CHECK(!revoked.value());
            auto read = t.invites.find(id<InviteId>(1));
            BOOST_REQUIRE(read && read.value() && read.value()->revokedAt);
            BOOST_CHECK_EQUAL(read.value()->revokedAt->time_since_epoch().count(), expected);
            BOOST_REQUIRE(t.context.commit());
        }
        Transaction t(*f.connection, f.active);
        BOOST_REQUIRE(t.context.begin());
        errorIs(t.invites.consume(inviteDigest(1), time(99)), ErrorCode::InviteUnavailable);
    }
}
}
