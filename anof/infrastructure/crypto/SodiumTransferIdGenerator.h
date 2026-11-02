#pragma once
#include <anof/application/ports/DriveIdGenerator.h>
#include <anof/application/ports/MaintenanceIdGenerator.h>
#include <anof/application/ports/TransferIdGenerator.h>

#include <memory>
namespace anof::infrastructure::crypto {
Result<std::unique_ptr<MaintenanceIdGenerator>> makeSodiumMaintenanceIdGenerator();
Result<std::unique_ptr<DriveIdGenerator>> makeSodiumDriveIdGenerator();
Result<std::unique_ptr<TransferIdGenerator>> makeSodiumTransferIdGenerator();
}  // namespace anof::infrastructure::crypto
