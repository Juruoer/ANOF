#pragma once

#include <anof/domain/identity/Session.h>
#include <anof/core/Page.h>

namespace anof {
class SessionReader {
public:
    virtual ~SessionReader() = default;
    // 仅计未撤销、未到期且 epoch 匹配的会话；调用方同事务检查上限并插入。
    virtual Result<SessionCount> countActive(UserId, UtcTime) = 0;
};
class SessionRepository : public SessionReader {
public:
    virtual Result<void> insert(const NewSession&) = 0;
    virtual Result<bool> revoke(const SessionCredential&, UserId, UtcTime) = 0;
    virtual Result<SessionCount> revokeAll(UserId, UtcTime) = 0;
    // 最多 size 条，按 expires_at/digest 顺序删除撤销、到期或 epoch 失效项。
    virtual Result<SessionCount> eraseInactive(UtcTime, PageSize size) = 0;
};
}  // namespace anof
