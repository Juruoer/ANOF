#pragma once

#include <anof/core/Id.h>

#include <optional>

namespace anof {

enum class SpaceKind { Private, Public };

struct Space {
    SpaceId id;
    SpaceKind kind;
    std::optional<UserId> ownerId;
    NodeId rootNodeId;
};

}  // namespace anof
