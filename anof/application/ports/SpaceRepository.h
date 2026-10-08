#pragma once

#include <anof/core/Result.h>
#include <anof/domain/drive/Space.h>

namespace anof {

class SpaceRepository {
public:
    virtual ~SpaceRepository() = default;
    virtual Result<std::optional<Space>> find(SpaceId id) = 0;
};

}  // namespace anof
