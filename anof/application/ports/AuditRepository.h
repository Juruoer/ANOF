#pragma once

#include <anof/core/Result.h>
#include <anof/domain/drive/Node.h>

namespace anof {

class AuditRepository {
public:
    virtual ~AuditRepository() = default;
    virtual Result<void> append(const DirectoryCreatedEvent& event) = 0;
};

}  // namespace anof
