#pragma once

#include <anof/core/Id.h>
#include <anof/infrastructure/crypto/internal/SodiumSupport.h>

#include <array>
#include <cstdint>

namespace anof::infrastructure::crypto::detail {

using RequestIdentitySeed = std::array<unsigned char, 32>;
using RequestIdentityDigest = std::array<unsigned char, 32>;

// 内部纯派生接缝；固定种子和哈希替身不进入公开工厂或全局状态。
RequestId requestIdFromDigest(const RequestIdentityDigest& digest);
RequestId deriveRequestId(const RequestIdentitySeed& seed, std::uint64_t connection,
                          std::uint64_t request, Sha256 sha256);

}  // namespace anof::infrastructure::crypto::detail
