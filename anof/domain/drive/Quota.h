#pragma once

#include <anof/core/Id.h>
#include <anof/core/Quantity.h>

#include <variant>

namespace anof {

struct SpaceQuota {
    SpaceId spaceId;
    friend bool operator==(const SpaceQuota& a, const SpaceQuota& b) { return a.spaceId == b.spaceId; }
};

struct PublicContributionQuota {
    SpaceId spaceId;
    UserId userId;
    friend bool operator==(const PublicContributionQuota& a, const PublicContributionQuota& b) {
        return a.spaceId == b.spaceId && a.userId == b.userId;
    }
};

using QuotaScope = std::variant<SpaceQuota, PublicContributionQuota>;

struct QuotaDelta {
    ByteCount bytes;
    NodeCount nodes;
};

struct QuotaState {
    QuotaDelta limit;
    QuotaDelta used;
    QuotaDelta reserved;
};

// 纯计算不修改输入；数据库端口必须在事务内将检查与更新原子化。
Result<QuotaState> chargeUsed(const QuotaState& state, QuotaDelta delta);

}  // namespace anof
