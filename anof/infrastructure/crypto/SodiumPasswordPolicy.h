#pragma once

#include <anof/domain/identity/Secrets.h>

namespace anof::infrastructure::crypto {

// 单次操作的适配器上限，不是生产推荐值或 worker 总内存预算。
struct SodiumPasswordPolicy {
    static constexpr std::uint32_t maxMemoryKiB = 262144;
    static constexpr std::uint32_t maxIterations = 10;
    // 无默认工作参数；零初始化必须在工厂验证时拒绝。
    PasswordHashParameters hashing{};
    PasswordHashLimits verification{};
};

// Argon2id v19；8..262144 KiB、1..10 次、parallelism=1。
// hashing 不得超过 verification；实现时在任何密码计算前校验。
Result<void> validateSodiumPasswordPolicy(const SodiumPasswordPolicy& policy);

}  // namespace anof::infrastructure::crypto
