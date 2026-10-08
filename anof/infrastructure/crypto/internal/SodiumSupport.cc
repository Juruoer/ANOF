#include <anof/infrastructure/crypto/internal/SodiumSupport.h>

#include <sodium.h>

namespace anof::infrastructure::crypto::detail {

Result<void> initializeSodiumWith(SodiumInitializer initialize) {
    if (!initialize) return ErrorCode::InternalError;
    const int status = initialize();
    if (status != 0 && status != 1) return ErrorCode::InternalError;
    return {};
}

Result<void> initializeSodium() {
    // libsodium 自身同步重复初始化；不缓存失败或替换系统随机源。
    return initializeSodiumWith(sodium_init);
}

}  // namespace anof::infrastructure::crypto::detail
