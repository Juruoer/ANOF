#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/core/Page.h>
#include <anof/domain/identity/IdentityValues.h>
#include <optional>

namespace anof {
struct InviteInfo {
    InviteId id;
    UserId createdBy;
    UtcTime createdAt;
    UtcTime expiresAt;
    InviteUseLimit maxUses;
    InviteUseCount usedCount;
    std::optional<UtcTime> revokedAt;
};
struct NewInvite {
    InviteInfo info;
    InviteDigest digest;
};
struct InviteListPosition {
    UtcTime createdAt;
    InviteId id;
};
}  // namespace anof
