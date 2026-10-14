#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/domain/identity/IdentityValues.h>
#include <anof/domain/identity/Secrets.h>

namespace anof {
inline constexpr std::int64_t kMaxActiveSessions = 32;
struct NewSession {
    SessionDigest digest;
    UserId userId;
    AuthEpoch authEpoch;
    CredentialMode mode;
    UtcTime createdAt;
    UtcTime expiresAt;
};
struct IssuedSessionToken {
    SecretToken text;
    SessionDigest digest;
};
struct IssuedInviteToken {
    SecretToken text;
    InviteDigest digest;
};
}  // namespace anof
