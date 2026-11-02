#pragma once
#include <anof/domain/transfer/TransferValues.h>
namespace anof {
class MaintenanceDigest {
public:
    virtual ~MaintenanceDigest() = default;
    virtual Result<Sha256> digest(std::string_view) = 0;
};
}  // namespace anof
