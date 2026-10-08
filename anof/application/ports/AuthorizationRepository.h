#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Result.h>
#include <anof/domain/identity/Principal.h>
#include <anof/domain/identity/SessionDigest.h>

namespace anof {

class AuthorizationRepository {
public:
    virtual ~AuthorizationRepository() = default;
    // 缺失、到期、撤销、禁用或 epoch 不符统一返回 Unauthenticated。
    virtual Result<Principal> requireActiveSession(const SessionDigest& digest, UtcTime now) = 0;
};

}  // namespace anof
