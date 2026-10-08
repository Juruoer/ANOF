#define BOOST_TEST_MODULE NodeIdGenerator
#include <boost/test/unit_test.hpp>

#include <anof/infrastructure/crypto/SodiumNodeIdGenerator.h>
#include <anof/infrastructure/crypto/internal/NodeUuid.h>

#include <array>
#include <set>
#include <string>

using namespace anof;
using namespace anof::infrastructure::crypto;

namespace {

void checkV4(const NodeId& id) {
    const auto text = id.toString();
    BOOST_REQUIRE_EQUAL(text.size(), 36U);
    BOOST_CHECK_EQUAL(text[14], '4');
    BOOST_CHECK(std::string("89ab").find(text[19]) != std::string::npos);
    BOOST_CHECK(text != "00000000-0000-0000-0000-000000000000");
    auto parsed = NodeId::parse(text);
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(parsed.value() == id);
}

}  // namespace

BOOST_AUTO_TEST_CASE(deterministic_formatting_and_non_nil) {
    std::array<unsigned char, 16> bytes{};
    auto zero = detail::nodeIdFromRandomBytes(bytes);
    BOOST_REQUIRE(zero);
    BOOST_CHECK_EQUAL(zero.value().toString(), "00000000-0000-4000-8000-000000000000");
    bytes.fill(255);
    auto ones = detail::nodeIdFromRandomBytes(bytes);
    BOOST_REQUIRE(ones);
    BOOST_CHECK_EQUAL(ones.value().toString(), "ffffffff-ffff-4fff-bfff-ffffffffffff");
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>(i);
    auto sequence = detail::nodeIdFromRandomBytes(bytes);
    BOOST_REQUIRE(sequence);
    BOOST_CHECK_EQUAL(sequence.value().toString(), "00010203-0405-4607-8809-0a0b0c0d0e0f");
    checkV4(zero.value());
    checkV4(ones.value());
    checkV4(sequence.value());
}

BOOST_AUTO_TEST_CASE(version_variant_masks_preserve_other_bits) {
    // 穷举两个被修改字节各自的全部输入，其余字节保持已知非零模式。
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned int value = 0; value < 256; ++value) {
        std::array<unsigned char, 16> bytes{};
        bytes.fill(0x5a);
        bytes[6] = static_cast<unsigned char>(value);
        bytes[8] = static_cast<unsigned char>(value);
        auto result = detail::nodeIdFromRandomBytes(bytes);
        BOOST_REQUIRE(result);
        std::string expected = "5a5a5a5a-5a5a-405a-805a-5a5a5a5a5a5a";
        expected[15] = hex[value & 15];
        expected[19] = "89ab"[(value >> 4) & 3];
        expected[20] = hex[value & 15];
        BOOST_CHECK_EQUAL(result.value().toString(), expected);
    }
}

BOOST_AUTO_TEST_CASE(bounded_production_sample) {
    auto generator = makeSodiumNodeIdGenerator();
    BOOST_REQUIRE(generator);
    BOOST_REQUIRE(generator.value());
    std::set<NodeId> seen;
    for (int i = 0; i < 128; ++i) {
        auto id = generator.value()->next();
        BOOST_REQUIRE(id);
        checkV4(id.value());
        // 只发现明显的固定输出／复用错误，不证明无碰撞或随机源质量。
        BOOST_CHECK(seen.insert(id.value()).second);
    }
}

BOOST_AUTO_TEST_CASE(repeated_factory_initialization) {
    for (int i = 0; i < 3; ++i) {
        auto generator = makeSodiumNodeIdGenerator();
        BOOST_REQUIRE(generator);
        BOOST_REQUIRE(generator.value());
        auto id = generator.value()->next();
        BOOST_REQUIRE(id);
        checkV4(id.value());
    }
}
