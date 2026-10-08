#include <anof/infrastructure/crypto/internal/RequestIdDerivation.h>

#include <sodium.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace anof::infrastructure::crypto::detail {
namespace {

constexpr std::string_view prefix = "ANOF/request-id/v1";

struct DerivationBuffers {
    std::array<unsigned char, prefix.size() + 32 + 16> input{};
    RequestIdentityDigest digest{};
    ~DerivationBuffers() {
        // input 含启动种子的副本，异常路径也必须清理。
        sodium_memzero(input.data(), input.size());
        sodium_memzero(digest.data(), digest.size());
    }
};

void writeBigEndian(unsigned char* output, std::uint64_t number) {
    for (unsigned int i = 0; i < 8; ++i) {
        output[i] = static_cast<unsigned char>(number >> (56 - 8 * i));
    }
}

}  // namespace

RequestId requestIdFromDigest(const RequestIdentityDigest& digest) {
    std::array<unsigned char, 16> bytes{};
    std::copy_n(digest.begin(), bytes.size(), bytes.begin());
    if (std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0; })) bytes.back() = 1;

    // 仅作 UUID 分组，不覆盖摘要中的 version/variant 位。
    constexpr char hex[] = "0123456789abcdef";
    std::array<char, 36> text{};
    std::size_t position = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text[position++] = '-';
        text[position++] = hex[bytes[i] >> 4];
        text[position++] = hex[bytes[i] & 15];
    }
    auto id = RequestId::parse(std::string_view(text.data(), text.size()));
    if (!id) throw std::logic_error("Invalid derived request ID");
    return id.value();
}

RequestId deriveRequestId(const RequestIdentitySeed& seed, std::uint64_t connection,
                          std::uint64_t request, Sha256 sha256) {
    DerivationBuffers buffers;
    auto* input = buffers.input.data();
    std::copy(prefix.begin(), prefix.end(), input);
    std::copy(seed.begin(), seed.end(), input + prefix.size());
    writeBigEndian(input + prefix.size() + seed.size(), connection);
    writeBigEndian(input + prefix.size() + seed.size() + 8, request);
    static_assert(crypto_hash_sha256_BYTES == 32);
    // 端口无 Result 通道。异常不得变成正常外观的替代 ID。
    if (!sha256 || sha256(buffers.digest.data(), input, buffers.input.size()) != 0) {
        throw std::runtime_error("Request ID derivation failed");
    }
    return requestIdFromDigest(buffers.digest);
}

}  // namespace anof::infrastructure::crypto::detail
