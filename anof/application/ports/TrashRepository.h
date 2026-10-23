#pragma once
#include <anof/domain/drive/Trash.h>
#include <anof/domain/identity/Principal.h>
namespace anof {
struct TrashQuery { Principal actor; SpaceId spaceId; std::optional<NodeId> root; PageSize limit; std::optional<TrashPosition> after; };
class TrashReader {
public:
    virtual ~TrashReader() = default;
    // 内部权威读取不代表授权；应用负责find后的空间及整项权限检查。
    virtual Result<std::optional<TrashEntry>> find(TrashId) = 0;
    virtual Result<Page<TrashEntry, TrashPosition>> list(const TrashQuery&) = 0;
    virtual Result<Page<TrashEntry, TrashExpiryPosition>> expired(UtcTime, PageSize, std::optional<TrashExpiryPosition>) = 0;
};
class TrashRepository : public TrashReader {
public:
    virtual Result<void> insert(const TrashEntry&) = 0;
    virtual Result<void> erase(TrashCondition) = 0;
};
}  // namespace anof
