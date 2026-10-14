#pragma once
#include <anof/infrastructure/crypto/SodiumPasswordPolicy.h>

namespace anof::runtime {
// 单一装配策略：64 MiB / t=2 / p=1。验证上限也供 Store、Bootstrap、Maintenance 使用。
// 尚未进行硬件校准；此值不定义在线密码 worker 的并发或队列准入。
inline infrastructure::crypto::SodiumPasswordPolicy instancePasswordPolicy() {
    return { { 65536, 2, 1 }, { 65536, 2, 1 } };
}
}  // namespace anof::runtime
