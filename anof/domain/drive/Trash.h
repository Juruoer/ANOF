#pragma once
#include <anof/domain/drive/NodeDetails.h>
#include <anof/domain/drive/Quota.h>

namespace anof {
struct NodeCondition {
    NodeId id;
    Revision revision;
};
struct TrashCondition {
    TrashId id;
    Revision revision;
};
enum class NodeKind { Directory,
                      File };
struct TrashEntry {
    TrashId id;
    SpaceId spaceId;
    NodeId rootNodeId;
    NodeId originalParentId;
    std::string originalName;
    NodeKind kind;
    UserId deletedBy;
    UtcTime deletedAt;
    UtcTime expiresAt;
    Revision revision;
    NodeCount nodeCount;
    ByteCount logicalBytes;
    std::optional<UserId> soleCreatorId;
};
struct TrashPosition {
    UtcTime deletedAt;
    TrashId id;
};
struct TrashExpiryPosition {
    UtcTime expiresAt;
    TrashId id;
};
inline constexpr std::int64_t kTrashRetentionSeconds = 30LL * 86400;
Result<void> validateTrashEntry(const TrashEntry&);
Result<UtcTime> trashExpiry(UtcTime deletedAt);
bool trashExpired(const TrashEntry&, UtcTime now) noexcept;
}  // namespace anof
