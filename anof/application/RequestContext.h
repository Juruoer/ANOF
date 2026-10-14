#pragma once

#include <anof/core/Id.h>
#include <anof/domain/identity/IdentityValues.h>

namespace anof {
struct RequestContext { SessionCredential credential; RequestId requestId; };
}  // namespace anof
