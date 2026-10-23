#pragma once

#include <anof/application/ports/AuthorizationReader.h>
#include <anof/application/ports/NodeAccountingRepository.h>
#include <anof/application/ports/UserRepository.h>
#include <anof/application/ports/SessionRepository.h>
#include <anof/application/ports/InviteRepository.h>
#include <anof/application/ports/SpaceRepository.h>
#include <anof/application/ports/NodeRepository.h>
#include <anof/application/ports/QuotaRepository.h>
#include <anof/application/ports/InstanceSettingsRepository.h>
#include <anof/application/ports/AuditRepository.h>
#include <anof/application/ports/IdentityAuditRepository.h>

namespace anof {
// 仓储共享本事务；不可复制/跨线程。未提交析构回滚，commit 无论结果都结束事务。
class MetadataTransaction {
public:
    MetadataTransaction() = default;
    MetadataTransaction(const MetadataTransaction&) = delete;
    MetadataTransaction& operator=(const MetadataTransaction&) = delete;
    virtual ~MetadataTransaction() = default;
    virtual AuthorizationReader& authorization() = 0;
    virtual UserRepository& users() = 0;
    virtual SessionRepository& sessions() = 0;
    virtual InviteRepository& invites() = 0;
    virtual SpaceRepository& spaces() = 0;
    virtual NodeRepository& nodes() = 0;
    virtual QuotaRepository& quotas() = 0;
    virtual SettingsRepository& settings() = 0;
    virtual AuditRepository& directoryAudit() = 0;
    virtual IdentityAuditRepository& identityAudit() = 0;
    virtual NodeAccountingRepository& nodeAccounting() = 0;
    virtual Result<void> commit() = 0;
};
}  // namespace anof
