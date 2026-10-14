#pragma once

#include <anof/domain/identity/IdentityValues.h>
#include <anof/domain/identity/Secrets.h>

namespace anof {
class InviteTokenCodec {
public:
    virtual ~InviteTokenCodec() = default;
    virtual Result<InviteDigest> digest(const InviteTokenText&) = 0;
};
}  // namespace anof
