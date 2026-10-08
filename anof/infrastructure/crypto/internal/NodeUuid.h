#pragma once

#include <anof/core/Id.h>

#include <array>

namespace anof::infrastructure::crypto::detail {

// 纯格式化接缝；生产生成器始终提供新鲜随机字节，不开放固定输入工厂。
Result<NodeId> nodeIdFromRandomBytes(std::array<unsigned char, 16> bytes);

}  // namespace anof::infrastructure::crypto::detail
