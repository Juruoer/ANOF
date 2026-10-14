#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Result.h>
#include <anof/domain/identity/Principal.h>
#include <anof/domain/identity/IdentityValues.h>

namespace anof {
class AuthorizationReader {
public:
    virtual ~AuthorizationReader() = default;
    // 缺失、模式不符、到期、撤销、禁用或 epoch 不符统一 Unauthenticated。
    virtual Result<Principal> requireActiveSession(const SessionCredential&, UtcTime) = 0;
};
}  // namespace anof
