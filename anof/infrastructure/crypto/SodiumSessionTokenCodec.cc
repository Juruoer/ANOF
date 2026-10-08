#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>
#include <anof/infrastructure/crypto/internal/SodiumSupport.h>

#include <sodium.h>

#include <array>
#include <new>

namespace anof::infrastructure::crypto {
namespace {

int base64urlValue(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

struct DecodedToken {
    std::array<unsigned char, 32> bytes{};
    ~DecodedToken() { sodium_memzero(bytes.data(), bytes.size()); }
};

class SodiumSessionTokenCodec final : public SessionTokenCodec {
public:
    Result<SessionDigest> digestBearer(std::string_view token) const override {
        return detail::digestBearerWith(token, crypto_hash_sha256);
    }
};

}  // namespace

Result<SessionDigest> detail::digestBearerWith(std::string_view token, Sha256 sha256) {
    // 先限制长度，再检查完整字母表和末尾两位，拒绝同一字节串的非规范编码。
    if (token.size() != 43) return ErrorCode::Unauthenticated;
    for (unsigned char c : token) {
        if (base64urlValue(c) < 0) return ErrorCode::Unauthenticated;
    }
    if ((base64urlValue(static_cast<unsigned char>(token.back())) & 3) != 0) {
        return ErrorCode::Unauthenticated;
    }

    DecodedToken decoded;
    std::size_t length = 0;
    const char* end = nullptr;
    const int status = sodium_base642bin(decoded.bytes.data(), decoded.bytes.size(), token.data(), token.size(),
                                        nullptr, &length, &end, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    // 输入已完成语法及规范性验证；解码器不满足后置条件属于内部失败。
    if (status != 0 || length != decoded.bytes.size() || end != token.data() + token.size()) {
        return ErrorCode::InternalError;
    }

    static_assert(crypto_hash_sha256_BYTES == 32);
    SessionDigest::Bytes digest{};
    if (!sha256 || sha256(digest.data(), decoded.bytes.data(), decoded.bytes.size()) != 0) {
        sodium_memzero(digest.data(), digest.size());
        return ErrorCode::InternalError;
    }
    return SessionDigest(digest);
}

Result<std::unique_ptr<SessionTokenCodec>> makeSodiumSessionTokenCodec() {
    auto initialized = detail::initializeSodium();
    if (!initialized) return initialized.error();
    try {
        return std::unique_ptr<SessionTokenCodec>(new SodiumSessionTokenCodec);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}

}  // namespace anof::infrastructure::crypto
