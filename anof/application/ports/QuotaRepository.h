#pragma once

#include <anof/core/Result.h>
#include <anof/domain/drive/Quota.h>

namespace anof {

class QuotaRepository {
public:
    virtual ~QuotaRepository() = default;
    // 缺失计费行属于 IntegrityError；所有作用域共享所属事务，不得内部 commit。
    virtual Result<void> chargeUsed(const QuotaScope& scope, QuotaDelta delta) = 0;
};

}  // namespace anof
