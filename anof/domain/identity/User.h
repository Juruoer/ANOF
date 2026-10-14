#pragma once

#include <anof/core/Clock.h>
#include <anof/domain/identity/IdentityValues.h>
#include <anof/domain/identity/Principal.h>
#include <anof/domain/identity/Secrets.h>

namespace anof {
struct UserProfile {
    UserId id;
    LoginName login;
    DisplayName displayName;
    UserRole role;
    bool enabled;
    AuthEpoch authEpoch;
    Revision revision;
    UtcTime createdAt;
    UtcTime updatedAt;
};
// 用户列表按规范 UserId 升序；位置只用于 seek，不携带身份或权限。
struct UserListPosition {
    UserId id;
};
struct PasswordSnapshot {
    UserId userId;
    EncodedPasswordHash encodedHash;
    PasswordVersion passwordVersion;
    AuthEpoch authEpoch;
    Revision userRevision;
    bool enabled;
};
struct PasswordExpectation {
    PasswordVersion passwordVersion;
    AuthEpoch authEpoch;
    Revision userRevision;
};
struct NewUser {
    UserProfile profile;
    EncodedPasswordHash encodedHash;
    PasswordVersion passwordVersion;
};
}  // namespace anof
