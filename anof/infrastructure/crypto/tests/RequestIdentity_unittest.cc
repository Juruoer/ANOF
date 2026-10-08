#define BOOST_TEST_MODULE RequestIdentity
#include <boost/test/unit_test.hpp>

#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <anof/infrastructure/crypto/internal/RequestIdDerivation.h>

#include <sodium.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace anof;
using namespace anof::infrastructure::crypto;

namespace {

struct Vector {
    int pattern;
    std::uint64_t connection;
    std::uint64_t request;
    const char* expected;
};

// Python hashlib.sha256 + int.to_bytes(8, 'big') + uuid.UUID(bytes=...) 独立生成。
constexpr Vector vectors[] = {
    { 0, 0ULL, 0ULL, "61d36332-22fa-5b0f-609a-40fbaaa09291" },
    { 0, 1ULL, 2ULL, "7cc73d71-c537-4280-5385-ddf7a7af5988" },
    { 0, 72623859790382856ULL, 1230066625199609624ULL, "72368c1b-e54a-0b75-d255-1f6a22ad6779" },
    { 0, 18446744073709551615ULL, 0ULL, "254094e2-9fa7-64d5-c2a8-0d1772b2c793" },
    { 0, 0ULL, 18446744073709551615ULL, "fe466ee8-5241-72df-8038-ddec4e5a9885" },
    { 0, 18446744073709551615ULL, 18446744073709551615ULL, "631a3454-02b3-ca98-5cf0-59af8c39c35f" },
    { 1, 0ULL, 0ULL, "4cd3ab57-3a09-00d9-a901-024aaf6ba4a6" },
    { 1, 1ULL, 2ULL, "16f7fc73-b80d-2987-126b-b81dbb603741" },
    { 1, 72623859790382856ULL, 1230066625199609624ULL, "6ab52498-f47d-3e45-54c7-2b6232c3dc7e" },
    { 1, 18446744073709551615ULL, 0ULL, "7afe22b2-b106-5e2a-c3b9-602db21b7e4e" },
    { 1, 0ULL, 18446744073709551615ULL, "84905e7a-dc00-e86d-dc49-a8087c30f92c" },
    { 1, 18446744073709551615ULL, 18446744073709551615ULL, "33ae0a35-edff-98e1-585e-0cee72813440" },
    { 2, 0ULL, 0ULL, "c7b0f947-b8ef-cece-ba4e-abde86d036e9" },
    { 2, 1ULL, 2ULL, "0ef1e374-07da-face-b83c-9886b9cb6195" },
    { 2, 72623859790382856ULL, 1230066625199609624ULL, "5389c823-5171-b3a1-4d47-d22d96d722f0" },
    { 2, 18446744073709551615ULL, 0ULL, "4b4ebbba-6222-ca4e-eaaa-e08d1e4811c5" },
    { 2, 0ULL, 18446744073709551615ULL, "f004fded-0b8b-69aa-b13f-9650de6d8a69" },
    { 2, 18446744073709551615ULL, 18446744073709551615ULL, "f81d4a5c-1cbd-bd79-0708-b7e12ae80ed4" },
};

detail::RequestIdentitySeed seedFor(int pattern) {
    detail::RequestIdentitySeed seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) {
        seed[i] = pattern == 0 ? 0 : pattern == 1 ? static_cast<unsigned char>(i) : 255;
    }
    return seed;
}

int inspectInput(unsigned char* output, const unsigned char* input, unsigned long long length) {
    BOOST_REQUIRE_EQUAL(length, 66U);
    BOOST_CHECK(std::string_view(reinterpret_cast<const char*>(input), 18) == "ANOF/request-id/v1");
    for (std::size_t i = 0; i < 32; ++i) BOOST_CHECK_EQUAL(input[18 + i], i);
    for (std::size_t i = 0; i < 8; ++i) {
        BOOST_CHECK_EQUAL(input[50 + i], i + 1);
        BOOST_CHECK_EQUAL(input[58 + i], i + 17);
    }
    std::fill_n(output, 32, 0);
    return 0;
}

int failedHash(unsigned char*, const unsigned char*, unsigned long long) { return -1; }

}  // namespace

BOOST_AUTO_TEST_CASE(known_answers_with_seed_and_exchange_boundaries) {
    BOOST_REQUIRE(detail::initializeSodium());
    for (const auto& vector : vectors) {
        auto id = detail::deriveRequestId(seedFor(vector.pattern), vector.connection, vector.request, crypto_hash_sha256);
        BOOST_CHECK_EQUAL(id.toString(), vector.expected);
        BOOST_CHECK(RequestId::parse(id.toString()));
    }
}

BOOST_AUTO_TEST_CASE(exact_prefix_seed_and_big_endian_layout) {
    BOOST_REQUIRE(detail::initializeSodium());
    auto id = detail::deriveRequestId(seedFor(1), 0x0102030405060708ULL, 0x1112131415161718ULL, inspectInput);
    BOOST_CHECK_EQUAL(id.toString(), "00000000-0000-0000-0000-000000000001");
}

BOOST_AUTO_TEST_CASE(zero_correction_uses_only_first_sixteen_bytes) {
    detail::RequestIdentityDigest digest{};
    BOOST_CHECK_EQUAL(detail::requestIdFromDigest(digest).toString(), "00000000-0000-0000-0000-000000000001");
    std::fill(digest.begin() + 16, digest.end(), 255);
    BOOST_CHECK_EQUAL(detail::requestIdFromDigest(digest).toString(), "00000000-0000-0000-0000-000000000001");
    digest[0] = 1;
    BOOST_CHECK_EQUAL(detail::requestIdFromDigest(digest).toString(), "01000000-0000-0000-0000-000000000000");
}

BOOST_AUTO_TEST_CASE(formatting_preserves_version_variant_and_truncates) {
    detail::RequestIdentityDigest digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) digest[i] = static_cast<unsigned char>(i);
    BOOST_CHECK_EQUAL(detail::requestIdFromDigest(digest).toString(), "00010203-0405-0607-0809-0a0b0c0d0e0f");
    digest.fill(255);
    BOOST_CHECK_EQUAL(detail::requestIdFromDigest(digest).toString(), "ffffffff-ffff-ffff-ffff-ffffffffffff");
}

BOOST_AUTO_TEST_CASE(hash_failure_has_no_fallback_id) {
    BOOST_REQUIRE(detail::initializeSodium());
    BOOST_CHECK_THROW(detail::deriveRequestId(seedFor(0), 0, 0, failedHash), std::runtime_error);
    BOOST_CHECK_THROW(detail::deriveRequestId(seedFor(0), 0, 0, nullptr), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(production_provider_reuses_seed_and_handles_boundaries) {
    auto provider = makeSodiumRequestIdentity();
    BOOST_REQUIRE(provider);
    BOOST_REQUIRE(provider.value());
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto first = provider.value()->forExchange(0, 0);
    const auto last = provider.value()->forExchange(maximum, maximum);
    BOOST_CHECK(RequestId::parse(first.toString()));
    BOOST_CHECK(RequestId::parse(last.toString()));
    for (std::uint64_t i = 0; i < 64; ++i) {
        auto id = provider.value()->forExchange(i, maximum - i);
        BOOST_CHECK(id == provider.value()->forExchange(i, maximum - i));
    }
    BOOST_CHECK(first == provider.value()->forExchange(0, 0));
    BOOST_CHECK(last == provider.value()->forExchange(maximum, maximum));
}

BOOST_AUTO_TEST_CASE(independent_production_providers) {
    auto first = makeSodiumRequestIdentity();
    auto second = makeSodiumRequestIdentity();
    BOOST_REQUIRE(first);
    BOOST_REQUIRE(second);
    // 小样本仅检查生产构造未明显复用固定种子，不证明无碰撞或随机源质量。
    BOOST_CHECK(first.value()->forExchange(1, 2) != second.value()->forExchange(1, 2));
}
