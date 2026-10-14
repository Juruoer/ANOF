#pragma once

#include <anof/application/ports/PasswordHasher.h>
#include <anof/infrastructure/crypto/SodiumPasswordPolicy.h>

#include <memory>

namespace anof::infrastructure::crypto {

// 工厂显式初始化 libsodium；成功对象非空，初始化失败无不安全回退。
Result<std::unique_ptr<PasswordHasher>> makeSodiumPasswordHasher(const SodiumPasswordPolicy& policy);

}  // namespace anof::infrastructure::crypto
