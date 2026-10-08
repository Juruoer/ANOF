#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <anof/infrastructure/crypto/internal/RequestIdDerivation.h>

#include <sodium.h>

#include <new>

namespace anof::infrastructure::crypto {
namespace {

class SodiumRequestIdentity final : public RequestIdentity {
public:
    // 只在完成 libsodium 初始化后构造；无固定种子生产入口。
    SodiumRequestIdentity() { randombytes_buf(seed_.data(), seed_.size()); }
    ~SodiumRequestIdentity() override { sodium_memzero(seed_.data(), seed_.size()); }
    SodiumRequestIdentity(const SodiumRequestIdentity&) = delete;
    SodiumRequestIdentity& operator=(const SodiumRequestIdentity&) = delete;

    RequestId forExchange(std::uint64_t connection, std::uint64_t request) const override {
        return detail::deriveRequestId(seed_, connection, request, crypto_hash_sha256);
    }

private:
    detail::RequestIdentitySeed seed_{};
};

}  // namespace

Result<std::unique_ptr<RequestIdentity>> makeSodiumRequestIdentity() {
    auto initialized = detail::initializeSodium();
    if (!initialized) return initialized.error();
    try {
        return std::unique_ptr<RequestIdentity>(new SodiumRequestIdentity);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}

}  // namespace anof::infrastructure::crypto
