#include <anof/infrastructure/crypto/SodiumInviteTokenCodec.h>
#include <anof/infrastructure/crypto/internal/SodiumSupport.h>
#include <sodium.h>
#include <new>

namespace anof::infrastructure::crypto {
namespace {
class SodiumInviteTokenCodec final : public InviteTokenCodec {
public:
    Result<InviteDigest> digest(const InviteTokenText& token) override {
        constexpr std::string_view prefix = "anof_invite_";
        const auto text = token.view();
        if (text.size() != prefix.size() + 43 || text.substr(0, prefix.size()) != prefix) return ErrorCode::InvalidArgument;
        // 复用固定长度、规范 base64url 检查；只摘要前缀后的原始随机字节。
        auto digest = detail::digestBearerWith(text.substr(prefix.size()), crypto_hash_sha256);
        if (!digest) return digest.error().code == ErrorCode::Unauthenticated ? Error{ ErrorCode::InvalidArgument } : digest.error();
        return InviteDigest(digest.value().bytes());
    }
};
}  // namespace
Result<std::unique_ptr<InviteTokenCodec>> makeSodiumInviteTokenCodec() {
    auto initialized = detail::initializeSodium();
    if (!initialized) return initialized.error();
    try { return std::unique_ptr<InviteTokenCodec>(new SodiumInviteTokenCodec); }
    catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}
}  // namespace anof::infrastructure::crypto
