#define BOOST_TEST_MODULE SessionTokenCodec
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/crypto/internal/SodiumSupport.h>
#include <sodium.h>

#include <array>
#include <boost/test/unit_test.hpp>
#include <string>
#include <string_view>

using namespace anof;
using namespace anof::infrastructure::crypto;

namespace {

struct Vector {
    const char* token;
    const char* digest;
};

// Python 标准库 base64.urlsafe_b64encode / hashlib.sha256 独立生成。
// 输入依次为 32 个零字节、0..31、32 个 0xff；不是编码文本的摘要。
constexpr Vector vectors[] = {
    { "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925" },
    { "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8", "630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd" },
    { "__________________________________________8", "af9613760f72635fbdb44a5a0a63c39f12af30f950a6ee5c971be188e89c4051" }
};

std::string hexDigest(const SessionDigest& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (auto byte : digest.bytes()) {
        text += hex[byte >> 4];
        text += hex[byte & 15];
    }
    return text;
}

void checkUnauthenticated(const SessionTokenCodec& codec, std::string_view token) {
    auto result = codec.digestBearer(token);
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == ErrorCode::Unauthenticated);
}

int firstInitialization() { return 0; }
int priorInitialization() { return 1; }
int failedInitialization() { return -1; }
int unexpectedInitialization() { return 2; }
int failedHash(unsigned char*, const unsigned char*, unsigned long long) { return -1; }
int forbiddenHash(unsigned char*, const unsigned char*, unsigned long long) {
    BOOST_FAIL("invalid input reached SHA-256");
    return -1;
}

}  // namespace

BOOST_AUTO_TEST_CASE(initialization_status_classification) {
    BOOST_CHECK(detail::initializeSodiumWith(firstInitialization));
    BOOST_CHECK(detail::initializeSodiumWith(priorInitialization));
    for (auto initialize : { failedInitialization, unexpectedInitialization, static_cast<detail::SodiumInitializer>(nullptr) }) {
        auto result = detail::initializeSodiumWith(initialize);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::InternalError);
    }
}

BOOST_AUTO_TEST_CASE(repeated_production_construction) {
    // 不重置进程全局状态；首次/重复状态的分类由上一个测试独立覆盖。
    for (int i = 0; i < 3; ++i) {
        auto codec = makeSodiumSessionTokenCodec();
        BOOST_REQUIRE(codec);
        BOOST_REQUIRE(codec.value());
        BOOST_CHECK(codec.value()->digestBearer(vectors[0].token));
    }
}

BOOST_AUTO_TEST_CASE(decoded_byte_known_answers) {
    auto codec = makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    for (const auto& vector : vectors) {
        auto digest = codec.value()->digestBearer(vector.token);
        BOOST_REQUIRE(digest);
        BOOST_CHECK_EQUAL(hexDigest(digest.value()), vector.digest);
    }
}

BOOST_AUTO_TEST_CASE(canonical_round_trips) {
    auto codec = makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    for (std::size_t v = 0; v < 3; ++v) {
        std::array<unsigned char, 32> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = v == 0 ? 0 : v == 1 ? static_cast<unsigned char>(i)
                                           : 255;
        }
        std::array<char, 44> encoded{};
        sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(), bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
        BOOST_CHECK_EQUAL(encoded.data(), vectors[v].token);
        auto digest = codec.value()->digestBearer(std::string_view(encoded.data(), 43));
        BOOST_REQUIRE(digest);
        BOOST_CHECK_EQUAL(hexDigest(digest.value()), vectors[v].digest);
    }
}

BOOST_AUTO_TEST_CASE(wrong_lengths_padding_scheme_and_trailing_input) {
    auto codec = makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    const std::string valid = vectors[0].token;
    for (const auto& token : { std::string{}, std::string(42, 'A'), std::string(44, 'A'), std::string(4096, 'A'),
                               valid + "=", valid + "==", valid + "A", valid + "\n", " " + valid,
                               "Bearer " + valid, std::string(vectors[0].digest), valid + std::string(1, '\0') }) {
        checkUnauthenticated(*codec.value(), token);
    }
    // string_view 不要求 NUL 结尾，也不得读取视图之外的尾随字节。
    const std::string surrounded = "!" + valid + "!";
    BOOST_CHECK(codec.value()->digestBearer(std::string_view(surrounded.data() + 1, 43)));
}

BOOST_AUTO_TEST_CASE(all_invalid_alphabet_bytes_at_each_position) {
    auto codec = makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (unsigned int byte = 0; byte < 256; ++byte) {
        const char c = static_cast<char>(byte);
        if (alphabet.find(c) != std::string::npos) continue;
        for (std::size_t position = 0; position < 43; ++position) {
            std::string token = vectors[0].token;
            token[position] = c;
            checkUnauthenticated(*codec.value(), token);
        }
    }
}

BOOST_AUTO_TEST_CASE(terminal_bits_and_urlsafe_alphabet) {
    auto codec = makeSodiumSessionTokenCodec();
    BOOST_REQUIRE(codec);
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        std::string token(43, alphabet[index]);
        token.back() = alphabet[index & ~std::size_t{ 3 }];
        BOOST_CHECK(codec.value()->digestBearer(token));
        token.back() = alphabet[index];
        auto result = codec.value()->digestBearer(token);
        if ((index & 3) == 0) {
            BOOST_CHECK(result);
        } else {
            BOOST_REQUIRE(!result);
            BOOST_CHECK(result.error().code == ErrorCode::Unauthenticated);
        }
    }
}

BOOST_AUTO_TEST_CASE(hash_failure_and_validation_order) {
    BOOST_REQUIRE(detail::initializeSodium());
    for (auto hash : { failedHash, static_cast<detail::Sha256>(nullptr) }) {
        auto result = detail::digestBearerWith(vectors[0].token, hash);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == ErrorCode::InternalError);
    }
    auto result = detail::digestBearerWith(std::string(43, '!'), forbiddenHash);
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == ErrorCode::Unauthenticated);
}
