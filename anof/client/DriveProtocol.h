#pragma once
#include <anof/client/Protocol.h>
namespace anof::client {
struct Trash {
    TrashId id;
    InstanceId instanceId;
    SpaceId spaceId;
    NodeId rootNodeId, originalParentId;
    std::string name;
    bool file;
    std::string deletedAt, expiresAt;
    Revision revision;
    std::string etag;
    NodeCount nodeCount;
    ByteCount logicalBytes;
};
enum class DriveOperation { Update,
                            Copy,
                            Remove,
                            Search,
                            ListTrash,
                            ShowTrash,
                            Restore,
                            Purge };
using DrivePayload = std::variant<Node, Trash, Page<Node>, Page<Trash>, Done>;
ClientResult<Reply<DrivePayload>> decodeDriveResponse(DriveOperation, const HttpResponse&, unsigned limit = 200);
ClientResult<ConditionTag> driveCondition(std::string_view);
}  // namespace anof::client
