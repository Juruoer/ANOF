#define BOOST_TEST_MODULE AnofPasswordHasher
#include <anof/infrastructure/crypto/SodiumPasswordHasher.h>
#include <anof/infrastructure/crypto/internal/PasswordWork.h>
#include <anof/infrastructure/crypto/internal/SecretBuffer.h>
#include <sodium.h>

#include <algorithm>
#include <boost/test/unit_test.hpp>
#include <limits>
#include <type_traits>

namespace anof::infrastructure::crypto {
namespace {
constexpr SodiumPasswordPolicy policy{ { 8192, 1, 1 }, { 8192, 1, 1 } };
constexpr const char* encoding = "$argon2id$v=19$m=8192,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
PasswordInput password(std::string_view text = "test-password-only") { return std::move(PasswordInput::parse(text)).value(); }
std::unique_ptr<PasswordHasher> hasher() {
    auto result = makeSodiumPasswordHasher(policy);
    BOOST_REQUIRE(result && result.value());
    return std::move(result).value();
}
template <typename T>
void errorIs(const Result<T>& result, ErrorCode code) {
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == code);
}
int calls = 0;
int failHash(char*, const char*, unsigned long long, unsigned long long, std::size_t) {
    ++calls;
    return -1;
}
int failDerive(unsigned char*, unsigned long long, const char*, unsigned long long, const unsigned char*, unsigned long long, std::size_t, int) {
    ++calls;
    return -1;
}
}  // namespace

BOOST_AUTO_TEST_CASE(explicit_policy_boundaries_are_validated_without_password_work) {
    errorIs(makeSodiumPasswordHasher({}), ErrorCode::InvalidArgument);
    for (const SodiumPasswordPolicy bad : {
             SodiumPasswordPolicy{ { 7, 1, 1 }, { 8192, 1, 1 } },
             { { 8192, 0, 1 }, { 8192, 1, 1 } },
             { { 8192, 1, 2 }, { 8192, 1, 2 } },
             { { 8192, 1, 1 }, { 8191, 1, 1 } },
             { { 8192, 2, 1 }, { 8192, 1, 1 } },
             { { 8192, 1, 1 }, { 262145, 1, 1 } },
             { { 8192, 1, 1 }, { 8192, 11, 1 } },
             { { 8192, 1, 1 }, { std::numeric_limits<std::uint32_t>::max(), 1, 1 } } }) {
        errorIs(makeSodiumPasswordHasher(bad), ErrorCode::InvalidArgument);
    }
    BOOST_REQUIRE(validateSodiumPasswordPolicy({ { 8, 1, 1 }, { 8, 1, 1 } }));
    BOOST_REQUIRE(validateSodiumPasswordPolicy({ { 262144, 10, 1 }, { 262144, 10, 1 } }));
    BOOST_REQUIRE(makeSodiumPasswordHasher(policy));
}

BOOST_AUTO_TEST_CASE(real_round_trip_wrong_password_and_sodium_interoperability) {
    auto h = hasher();
    auto input = password();
    auto first = h->hash(input);
    auto second = h->hash(input);
    BOOST_REQUIRE(first && second);
    BOOST_CHECK(first.value().view() != second.value().view());
    BOOST_CHECK_EQUAL(first.value().parameters().memoryKiB, 8192);
    auto valid = h->verify(input, first.value());
    auto wrong = h->verify(password("another-password"), first.value());
    BOOST_REQUIRE(valid && wrong);
    BOOST_CHECK(valid.value() && !wrong.value());
    detail::SecretBuffer<char, crypto_pwhash_argon2id_STRBYTES> terminated;
    std::copy(first.value().view().begin(), first.value().view().end(), terminated.bytes.begin());
    BOOST_CHECK_EQUAL(crypto_pwhash_argon2id_str_verify(terminated.bytes.data(), input.view().data(), input.view().size()), 0);
}

BOOST_AUTO_TEST_CASE(password_boundaries_are_exact_without_trimming_or_normalizing) {
    auto h = hasher();
    for (const auto& text : { std::string("123456789012"), std::string(1024, 'x'), std::string("  密码 test  ") }) {
        auto input = password(text);
        auto encoded = h->hash(input);
        BOOST_REQUIRE(encoded);
        auto checked = h->verify(input, encoded.value());
        BOOST_REQUIRE(checked && checked.value());
    }
    auto input = password("  padded-password  ");
    auto encoded = h->hash(input);
    BOOST_REQUIRE(encoded);
    auto trimmed = h->verify(password("padded-password"), encoded.value());
    BOOST_REQUIRE(trimmed);
    BOOST_CHECK(!trimmed.value());
}

BOOST_AUTO_TEST_CASE(over_budget_phc_never_reaches_expensive_work) {
    auto h = hasher();
    const std::string base(encoding);
    for (const auto& replacement : { std::string("m=8193,t=1,p=1"), std::string("m=8192,t=2,p=1"), std::string("m=8192,t=1,p=2") }) {
        auto text = base;
        const auto start = text.find("m=");
        text.replace(start, text.find('$', start) - start, replacement);
        auto encoded = EncodedPasswordHash::parse(text, { 262144, 10, 2 });
        BOOST_REQUIRE(encoded);
        calls = 0;
        errorIs(detail::verifyPasswordWith(password(), encoded.value(), policy, failDerive), ErrorCode::InvalidArgument);
        BOOST_CHECK_EQUAL(calls, 0);
        errorIs(h->verify(password(), encoded.value()), ErrorCode::InvalidArgument);
    }
}

BOOST_AUTO_TEST_CASE(domain_supported_but_adapter_unsupported_salt_and_hash_lengths_are_rejected) {
    auto h = hasher();
    for (bool salt : { false, true }) {
        std::string text(encoding);
        if (salt)
            text.insert(text.find('$', text.find('$', 16) + 1), 2, 'A');
        else
            text.append(1, 'A');
        auto encoded = EncodedPasswordHash::parse(text, policy.verification);
        BOOST_REQUIRE(encoded);
        calls = 0;
        errorIs(detail::verifyPasswordWith(password(), encoded.value(), policy, failDerive), ErrorCode::InvalidArgument);
        BOOST_CHECK_EQUAL(calls, 0);
    }
    for (const auto& text : { std::string{}, std::string("$argon2i$v=19$m=8192,t=1,p=1$invalid"), std::string(513, 'x'), std::string(encoding) + "=" }) {
        errorIs(EncodedPasswordHash::parse(text, policy.verification), ErrorCode::InvalidArgument);
    }
}

BOOST_AUTO_TEST_CASE(calculation_failures_and_bad_hash_postconditions_are_internal_errors) {
    auto h = hasher();
    auto encoded = EncodedPasswordHash::parse(encoding, policy.verification);
    BOOST_REQUIRE(encoded);
    calls = 0;
    errorIs(detail::hashPasswordWith(password(), policy, failHash), ErrorCode::InternalError);
    errorIs(detail::verifyPasswordWith(password(), encoded.value(), policy, failDerive), ErrorCode::InternalError);
    BOOST_CHECK_EQUAL(calls, 2);
    errorIs(detail::hashPasswordWith(password(), policy, nullptr), ErrorCode::InternalError);
    errorIs(detail::verifyPasswordWith(password(), encoded.value(), policy, nullptr), ErrorCode::InternalError);
    auto unterminated = [](char* out, const char*, unsigned long long, unsigned long long, std::size_t) {
        std::fill_n(out, crypto_pwhash_argon2id_STRBYTES, 'x');
        return 0;
    };
    errorIs(detail::hashPasswordWith(password(), policy, unterminated), ErrorCode::InternalError);
    auto malformed = [](char*, const char*, unsigned long long, unsigned long long, std::size_t) { return 0; };
    errorIs(detail::hashPasswordWith(password(), policy, malformed), ErrorCode::InternalError);
}

BOOST_AUTO_TEST_CASE(moved_from_secret_values_are_rejected_and_owned_results_survive) {
    static_assert(!std::is_copy_constructible_v<PasswordInput>);
    static_assert(!std::is_copy_constructible_v<EncodedPasswordHash>);
    auto h = hasher();
    auto input = password();
    auto moved = std::move(input);
    errorIs(h->hash(input), ErrorCode::InvalidArgument);
    auto encoded = h->hash(moved);
    BOOST_REQUIRE(encoded);
    auto owned = std::move(encoded).value();
    errorIs(h->verify(moved, encoded.value()), ErrorCode::InvalidArgument);
    h.reset();
    auto other = hasher();
    BOOST_REQUIRE(other->verify(moved, owned).value());
}
}  // namespace anof::infrastructure::crypto
