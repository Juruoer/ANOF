#pragma once

#include <anof/core/Id.h>
#include <anof/core/Quantity.h>

namespace anof {

enum class UserRole { Member, Administrator };

// 仅表示当前事务内已验证身份，不允许长期缓存作为授权依据。
struct Principal {
    UserId userId;
    UserRole role;
    AuthEpoch authEpoch;
};

}  // namespace anof
