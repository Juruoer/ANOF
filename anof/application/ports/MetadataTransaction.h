#pragma once

#include <anof/application/ports/AuditRepository.h>
#include <anof/application/ports/AuthorizationRepository.h>
#include <anof/application/ports/NodeRepository.h>
#include <anof/application/ports/QuotaRepository.h>
#include <anof/application/ports/SpaceRepository.h>

namespace anof {

// 不可复制／跨线程；仓储引用只在本事务打开期间有效。
// 析构回滚未提交变更且不抛异常。commit 无论成功或失败都结束本事务；不得重用。
class MetadataTransaction {
public:
    MetadataTransaction() = default;
    MetadataTransaction(const MetadataTransaction&) = delete;
    MetadataTransaction& operator=(const MetadataTransaction&) = delete;
    virtual ~MetadataTransaction() = default;

    virtual AuthorizationRepository& authorization() = 0;
    virtual SpaceRepository& spaces() = 0;
    virtual NodeRepository& nodes() = 0;
    virtual QuotaRepository& quotas() = 0;
    virtual AuditRepository& audit() = 0;
    // 成功必须满足持久化契约；无法判定提交结果返回 CommitOutcomeUnknown。
    virtual Result<void> commit() = 0;
};

}  // namespace anof
