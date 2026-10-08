#include <anof/infrastructure/crypto/SodiumNodeIdGenerator.h>
#include <anof/infrastructure/crypto/internal/NodeUuid.h>
#include <anof/infrastructure/crypto/internal/SodiumSupport.h>

#include <sodium.h>

#include <array>
#include <new>

namespace anof::infrastructure::crypto {
namespace {

class SodiumNodeIdGenerator final : public NodeIdGenerator {
public:
    Result<NodeId> next() override {
        std::array<unsigned char, 16> bytes{};
        // 无私有 PRNG 或请求 ID 种子；每次直接取 libsodium 安全随机源。
        // randombytes_buf 无错误返回通道，库级致命失败不能转换为 Result。
        randombytes_buf(bytes.data(), bytes.size());
        return detail::nodeIdFromRandomBytes(bytes);
    }
};

}  // namespace

Result<std::unique_ptr<NodeIdGenerator>> makeSodiumNodeIdGenerator() {
    auto initialized = detail::initializeSodium();
    if (!initialized) return initialized.error();
    try {
        return std::unique_ptr<NodeIdGenerator>(new SodiumNodeIdGenerator);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}

}  // namespace anof::infrastructure::crypto
