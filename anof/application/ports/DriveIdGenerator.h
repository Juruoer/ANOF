#pragma once
#include <anof/core/Id.h>
namespace anof {
class DriveIdGenerator {
public:
    virtual ~DriveIdGenerator() = default;
    virtual Result<TrashId> trashId() = 0;
};
}  // namespace anof
