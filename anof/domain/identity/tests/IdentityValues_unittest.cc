#define BOOST_TEST_MODULE AnofIdentityValues
#include <anof/core/Page.h>
#include <anof/domain/identity/IdentityValues.h>
#include <anof/domain/identity/Secrets.h>

#include <boost/test/unit_test.hpp>
#include <limits>
#include <type_traits>

using namespace anof;
static_assert(!std::is_copy_constructible_v<PasswordInput>);
static_assert(!std::is_copy_constructible_v<SecretToken>);
static_assert(!std::is_copy_constructible_v<InviteTokenText>);
static_assert(!std::is_copy_constructible_v<EncodedPasswordHash>);
static_assert(!std::is_convertible_v<SessionDigest, SessionCredential>);
static_assert(!std::is_convertible_v<InviteDigest, SessionDigest>);

BOOST_AUTO_TEST_CASE(login_and_display_validation) {
    BOOST_REQUIRE(LoginName::parse("Admin_01"));
    BOOST_CHECK_EQUAL(LoginName::parse("Admin_01").value().value(), "admin_01");
    for (const auto* value : { "ab", "1abc", " abc", "abc ", "a.b", "管理员" })
        BOOST_CHECK(!LoginName::parse(value));
    BOOST_CHECK(LoginName::parse(std::string(32, 'a')));
    BOOST_CHECK(!LoginName::parse(std::string(33, 'a')));
    BOOST_CHECK(DisplayName::parse("家庭 云盘"));
    BOOST_CHECK(DisplayName::parse(std::string(128, 'a')));
    BOOST_CHECK(!DisplayName::parse(std::string(129, 'a')));
    for (const std::string value : { std::string{}, std::string("a\0b", 3), std::string("a\nb"),
                                     std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                                     std::string("\xe4\xb8"), std::string("\xc2\x85") }) BOOST_CHECK(!DisplayName::parse(value));
}
BOOST_AUTO_TEST_CASE(password_bytes_and_move_ownership) {
    BOOST_CHECK(!PasswordInput::parse(std::string(11, 'x')));
    BOOST_CHECK(PasswordInput::parse(std::string(1024, 'x')));
    BOOST_CHECK(!PasswordInput::parse(std::string(1025, 'x')));
    BOOST_CHECK(!PasswordInput::parse(std::string(12, '\0')));
    BOOST_CHECK(!PasswordInput::parse(std::string(12, '\xff')));
    BOOST_CHECK(PasswordInput::parse("密码密码"));  // 12 UTF-8 字节。
    auto first = PasswordInput::parse("  Password\n ").value();
    const auto original = first.view().data();
    auto second = std::move(first);
    BOOST_CHECK(first.view().empty());
    BOOST_CHECK(second.view().data() == original);
    BOOST_CHECK_EQUAL(second.view(), "  Password\n ");
    auto third = PasswordInput::parse("another password").value();
    third = std::move(second);
    BOOST_CHECK(second.view().empty());
    BOOST_CHECK(third.view().data() == original);
}
BOOST_AUTO_TEST_CASE(credentials_are_canonical_and_distinct) {
    const std::string raw(43, 'A');
    BOOST_CHECK(SecretToken::session(raw));
    BOOST_CHECK(!SecretToken::session(raw + "="));
    BOOST_CHECK(!SecretToken::session(std::string(42, 'A') + "B"));
    BOOST_CHECK(!SecretToken::session(std::string(42, 'A') + "/"));
    BOOST_CHECK(InviteTokenText::parse("anof_invite_" + raw));
    BOOST_CHECK(SecretToken::invite("anof_invite_" + raw));
    BOOST_CHECK(!InviteTokenText::parse(raw));
    BOOST_CHECK(!SecretToken::session("anof_invite_" + raw));
}
BOOST_AUTO_TEST_CASE(password_encoding_is_bounded_before_crypto) {
    const std::string suffix = "$" + std::string(22, 'A') + "$" + std::string(43, 'A');
    const PasswordHashLimits limits{ 65536, 3, 1 };
    auto hash = EncodedPasswordHash::parse("$argon2id$v=19$m=65536,t=2,p=1" + suffix, limits);
    BOOST_REQUIRE(hash);
    BOOST_CHECK_EQUAL(hash.value().parameters().memoryKiB, 65536U);
    for (const auto* prefix : { "$argon2i$v=19$m=65536,t=2,p=1", "$argon2id$v=16$m=65536,t=2,p=1",
                                "$argon2id$v=19$m=65537,t=2,p=1", "$argon2id$v=19$m=65536,t=4,p=1",
                                "$argon2id$v=19$m=65536,t=2,p=2", "$argon2id$v=19$m=7,t=2,p=1",
                                "$argon2id$v=19$m=4294967296,t=2,p=1", "$argon2id$v=19$m=065536,t=2,p=1" })
        BOOST_CHECK(!EncodedPasswordHash::parse(std::string(prefix) + suffix, limits));
    BOOST_CHECK(!EncodedPasswordHash::parse("$argon2id$v=19$m=65536,t=2,p=1$AA$AA", limits));
    BOOST_CHECK(!EncodedPasswordHash::parse(std::string(hash.value().view()) + "=", limits));
    BOOST_CHECK(!EncodedPasswordHash::parse(std::string(513, 'A'), limits));
}
BOOST_AUTO_TEST_CASE(quantities_enforce_bounds_and_checked_arithmetic) {
    BOOST_CHECK(!PageSize::from(0));
    BOOST_CHECK(PageSize::from(200));
    BOOST_CHECK(!PageSize::from(201));
    BOOST_CHECK(!PageSize::from(200).value().add(1));
    BOOST_CHECK(!PageSize{}.add(std::numeric_limits<std::int64_t>::max()));
    BOOST_CHECK(!UserLimit::from(10001));
    BOOST_CHECK(!PasswordVersion::from(0));
    BOOST_CHECK(!Revision::from(0));
    BOOST_CHECK(!ByteCount::from(-1));
    auto maximum = ByteCount::from(std::numeric_limits<std::int64_t>::max()).value();
    BOOST_CHECK(!maximum.add(1));
    BOOST_CHECK(ByteCount::from(1LL << 40));
}
