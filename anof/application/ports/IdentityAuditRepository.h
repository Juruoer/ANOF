#pragma once

#include <anof/domain/identity/IdentityEvent.h>
#include <anof/core/Result.h>

namespace anof {
class IdentityAuditRepository {
public:
    virtual ~IdentityAuditRepository() = default;
    virtual Result<void> append(const IdentityEvent&) = 0;
};
}  // namespace anof
