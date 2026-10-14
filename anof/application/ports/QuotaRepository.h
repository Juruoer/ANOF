#pragma once

#include <anof/core/Result.h>
#include <anof/domain/drive/Quota.h>

namespace anof {
class QuotaReader {
public:
    virtual ~QuotaReader() = default;
    virtual Result<QuotaSnapshot> get(const QuotaScope&) = 0;
};
class QuotaRepository : public QuotaReader {
public:
    virtual Result<void> insert(const QuotaScope&, const QuotaSnapshot&) = 0;
    // 下调不得低于 used+reserved；条件不符 PreconditionFailed，实际变更递增 revision。
    virtual Result<QuotaSnapshot> setLimit(const QuotaScope&, Revision expected, QuotaDelta) = 0;
    // 所有额度写入共享所属事务；缺失额度行 IntegrityError，算术/版本溢出整体失败。
    virtual Result<void> chargeUsed(const QuotaScope&, QuotaDelta) = 0;
};
}  // namespace anof
