#pragma once

#include <anof/infrastructure/crypto/SodiumPasswordPolicy.h>

#include <cstddef>

namespace anof::infrastructure::crypto::detail {
// 逐次调用的测试接缝；不替换全局算法，不提供可注入的生产工厂。
using HashString = int (*)(char*, const char*, unsigned long long, unsigned long long, std::size_t);
using DerivePassword = int (*)(unsigned char*, unsigned long long, const char*, unsigned long long,
                               const unsigned char*, unsigned long long, std::size_t, int);
Result<EncodedPasswordHash> hashPasswordWith(const PasswordInput&, const SodiumPasswordPolicy&, HashString);
Result<bool> verifyPasswordWith(const PasswordInput&, const EncodedPasswordHash&, const SodiumPasswordPolicy&, DerivePassword);
}  // namespace anof::infrastructure::crypto::detail
