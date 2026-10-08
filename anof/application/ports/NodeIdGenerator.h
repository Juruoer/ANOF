#pragma once

#include <anof/core/Id.h>

namespace anof {

class NodeIdGenerator {
public:
    virtual ~NodeIdGenerator() = default;
    virtual Result<NodeId> next() = 0;
};

}  // namespace anof
