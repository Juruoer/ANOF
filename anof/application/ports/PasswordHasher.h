#pragma once

#include <anof/domain/identity/Secrets.h>

namespace anof {
class PasswordHasher {
public:
    virtual ~PasswordHasher() = default;
    // 仅在线密码 worker 或离线维护调用线程使用。编码结构校验不等于密码已验证，执行前仍检查适配器预算。
    virtual Result<EncodedPasswordHash> hash(const PasswordInput&) = 0;
    virtual Result<bool> verify(const PasswordInput&, const EncodedPasswordHash&) = 0;
};
}  // namespace anof
