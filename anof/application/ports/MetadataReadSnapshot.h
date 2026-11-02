#pragma once

#include <anof/application/ports/AuthorizationReader.h>
#include <anof/application/ports/InstanceSettingsRepository.h>
#include <anof/application/ports/InviteRepository.h>
#include <anof/application/ports/NodeRepository.h>
#include <anof/application/ports/QuotaRepository.h>
#include <anof/application/ports/SessionRepository.h>
#include <anof/application/ports/SpaceRepository.h>
#include <anof/application/ports/UserRepository.h>

namespace anof {
// 仓储引用仅在本快照生存期有效；不可复制/跨线程，析构结束读事务。
class MetadataReadSnapshot {
public:
    MetadataReadSnapshot() = default;
    MetadataReadSnapshot(const MetadataReadSnapshot&) = delete;
    MetadataReadSnapshot& operator=(const MetadataReadSnapshot&) = delete;
    virtual ~MetadataReadSnapshot() = default;
    virtual AuthorizationReader& authorization() = 0;
    virtual UserReader& users() = 0;
    virtual SessionReader& sessions() = 0;
    virtual InviteReader& invites() = 0;
    virtual SpaceReader& spaces() = 0;
    virtual NodeReader& nodes() = 0;
    virtual QuotaReader& quotas() = 0;
    virtual SettingsReader& settings() = 0;
};
}  // namespace anof
