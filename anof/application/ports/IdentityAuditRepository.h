#pragma once

#include <anof/core/Result.h>
#include <anof/domain/identity/IdentityEvent.h>

namespace anof {
class IdentityAuditRepository {
public:
    virtual ~IdentityAuditRepository() = default;
    virtual Result<void> append(const IdentityEvent&) = 0;
};
}  // namespace anof
