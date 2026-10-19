#pragma once

#include <anof/domain/identity/Invite.h>

namespace anof {
class InviteReader {
public:
    virtual ~InviteReader() = default;
    virtual Result<std::optional<InviteInfo>> find(InviteId) = 0;
    virtual Result<Page<InviteInfo, InviteListPosition>> list(PageSize, std::optional<InviteListPosition> after) = 0;
};
class InviteRepository : public InviteReader {
public:
    virtual Result<void> insert(const NewInvite&) = 0;
    // 条件增次数；未知/到期/撤销/耗尽统一 InviteUnavailable。注册失败须同事务回滚。
    virtual Result<InviteInfo> consume(const InviteDigest&, UtcTime) = 0;
    // 已撤销为 false且保留原时间；未知 ID 为 NotFound，不删除历史。
    // 首次撤销时间=max(原始观测UTC, createdAt)，不改变consume的原始UTC到期判断。
    virtual Result<bool> revoke(InviteId, UtcTime) = 0;
};
}  // namespace anof
