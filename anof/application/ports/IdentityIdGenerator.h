#pragma once

#include <anof/core/Id.h>

namespace anof {
class IdentityIdGenerator {
public:
    virtual ~IdentityIdGenerator() = default;
    virtual Result<UserId> nextUser() = 0;
    virtual Result<SpaceId> nextSpace() = 0;
    virtual Result<NodeId> nextNode() = 0;
    virtual Result<InviteId> nextInvite() = 0;
    virtual Result<InstanceId> nextInstance() = 0;
    virtual Result<OperationId> nextOperation() = 0;
};
}  // namespace anof
