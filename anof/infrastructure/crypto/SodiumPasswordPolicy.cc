#include <anof/infrastructure/crypto/SodiumPasswordPolicy.h>
#include <sodium.h>
#include <limits>

namespace anof::infrastructure::crypto {
namespace {
bool supported(std::uint32_t memoryKiB, std::uint32_t iterations, std::uint32_t parallelism) {
    if (memoryKiB < 8 || memoryKiB > SodiumPasswordPolicy::maxMemoryKiB ||
        iterations < 1 || iterations > SodiumPasswordPolicy::maxIterations || parallelism != 1) return false;
    if (memoryKiB > std::numeric_limits<std::size_t>::max() / 1024) return false;
    const auto bytes = static_cast<std::size_t>(memoryKiB) * 1024;
    return bytes >= crypto_pwhash_argon2id_MEMLIMIT_MIN && bytes <= crypto_pwhash_argon2id_MEMLIMIT_MAX &&
           iterations >= crypto_pwhash_argon2id_OPSLIMIT_MIN && iterations <= crypto_pwhash_argon2id_OPSLIMIT_MAX;
}
}  // namespace
Result<void> validateSodiumPasswordPolicy(const SodiumPasswordPolicy& policy) {
    const auto& h = policy.hashing;
    const auto& v = policy.verification;
    if (!supported(h.memoryKiB, h.iterations, h.parallelism) || !supported(v.maxMemoryKiB, v.maxIterations, v.maxParallelism) ||
        h.memoryKiB > v.maxMemoryKiB || h.iterations > v.maxIterations) return ErrorCode::InvalidArgument;
    return {};
}
}  // namespace anof::infrastructure::crypto
