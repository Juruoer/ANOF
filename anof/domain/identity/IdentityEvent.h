#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/domain/drive/Quota.h>

#include <variant>

namespace anof {
struct InstanceInitialized {
    UserId administrator;
    InstanceId instance;
};
struct InviteCreated {
    UserId actor;
    InviteId invite;
};
struct InviteRevoked {
    UserId actor;
    InviteId invite;
};
struct UserRegistered {
    UserId subject;
    InviteId invite;
};
struct SessionCreated {
    UserId subject;
};
struct SessionRevoked {
    UserId subject;
};
struct AdministratorPasswordReset {
    UserId subject;
};
struct PasswordChanged {
    UserId subject;
};
struct UserEnabled {
    UserId actor;
    UserId subject;
};
struct UserDisabled {
    UserId actor;
    UserId subject;
};
struct DisplayNameChanged {
    UserId subject;
};
struct QuotaChanged {
    UserId actor;
    QuotaScope scope;
};
using IdentityAction = std::variant<InstanceInitialized, InviteCreated, InviteRevoked, UserRegistered,
                                    SessionCreated, SessionRevoked, PasswordChanged, UserEnabled, UserDisabled, DisplayNameChanged, QuotaChanged, AdministratorPasswordReset>;
// 行为 variant 强制所需归属字段；不接收任意 action 字符串、JSON 或秘密。
struct IdentityEvent {
    RequestId requestId;
    UtcTime occurredAt;
    IdentityAction action;
};
}  // namespace anof
