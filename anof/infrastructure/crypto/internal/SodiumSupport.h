#pragma once

#include <anof/core/Result.h>
#include <anof/domain/identity/SessionDigest.h>

#include <string_view>

namespace anof::infrastructure::crypto::detail {

// 内部测试接缝只接受逐次调用的函数，不安装全局回调、不改变 libsodium 状态。
using SodiumInitializer = int (*)();
using Sha256 = int (*)(unsigned char*, const unsigned char*, unsigned long long);
Result<void> initializeSodium();
Result<void> initializeSodiumWith(SodiumInitializer initialize);

// 仅供适配器及局部测试使用；调用方必须已完成初始化。
Result<SessionDigest> digestBearerWith(std::string_view token, Sha256 sha256);

}  // namespace anof::infrastructure::crypto::detail
