#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/core/Quantity.h>

#include <optional>
#include <string>
#include <vector>

namespace anof {

struct DirectorySnapshot {
    NodeId id;
    SpaceId spaceId;
    std::optional<NodeId> parentId;
    std::string name;
    UserId createdBy;
    Revision revision;
    UtcTime createdAt;
    UtcTime updatedAt;
};

struct NewDirectory {
    NodeId id;
    SpaceId spaceId;
    NodeId parentId;
    std::string name;
    UserId createdBy;
    UtcTime createdAt;
};

// 从空间根到目标父目录。适配层保证全部活动、全部目录且无隐藏的回收站祖先。
using DirectoryPath = std::vector<DirectorySnapshot>;

struct DirectoryCreatedEvent {
    UserId actor;
    SpaceId spaceId;
    NodeId nodeId;
    NodeId parentId;
    RequestId requestId;
    UtcTime occurredAt;
};

}  // namespace anof
