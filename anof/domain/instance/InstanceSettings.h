#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/domain/drive/Quota.h>
#include <anof/domain/identity/IdentityValues.h>

namespace anof {
struct InstanceSettings {
    InstanceId instanceId;
    OperationId bootstrapOperationId;
    UtcTime initializedAt;
    UserLimit maxUsers;
    QuotaDelta defaultPrivate;
    QuotaDelta defaultContribution;
    Revision revision;
};
}  // namespace anof
