#pragma once

#include <anof/domain/identity/Session.h>

namespace anof {
class CredentialIssuer {
public:
    virtual ~CredentialIssuer() = default;
    // 独立安全随机值，返回明文自有秘密及摘要；不得在 selector 调用。
    virtual Result<IssuedSessionToken> issueSession() = 0;
    virtual Result<IssuedInviteToken> issueInvite() = 0;
};
}  // namespace anof
