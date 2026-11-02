#pragma once
#include <anof/application/ports/MaintenanceDigest.h>
namespace anof::infrastructure::crypto {
class SodiumMaintenanceDigest final : public MaintenanceDigest {
public:
    Result<Sha256> digest(std::string_view) override;
};
}  // namespace anof::infrastructure::crypto
