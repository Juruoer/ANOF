#pragma once
#include <anof/application/ports/MaintenanceFileStore.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>
#include <anof/infrastructure/system/internal/MaintenanceIo.h>
namespace anof::infrastructure::system::maintenance {
using detail::OwnedFd;
MaintenanceFailure failure(const MaintenanceContext&, MaintenanceStage, MaintenanceError, MaintenanceOutcome = MaintenanceOutcome::NotStarted);
MaintenanceFailure ioFailure(const MaintenanceContext&, MaintenanceStage, bool effect = false);
Result<OwnedFd> directory(int parent, const std::string& name, bool create = false);
Result<FileEvidence> evidence(int fd);
bool same(const FileEvidence&, const FileEvidence&);
Result<BackupFileDigest> hashCopy(const MaintenanceContext&, int source, int destination = -1, Io* io = nullptr);
Result<void> writeAll(int, const void*, std::size_t, Io* io = nullptr);
struct Location {
    OwnedFd parent;
    std::string name;
};
Result<Location> locate(const InstanceLease&, const MaintenanceFileKey&, bool create = false);
class Destination final : public MaintenanceDestination {
public:
    std::filesystem::path finalPath, stagePath;
    OwnedFd parent, lock, stage;
    mutable BackupIndexValidator index;
    mutable std::optional<BackupObjectIndex> finishedIndex;
    mutable NodeCount indexCount;
    mutable ByteCount indexBytes;
    mutable bool sealed = false;
    mutable bool failed = false;
    mutable std::optional<BackupManifest> restoreManifest;
    mutable std::optional<InstanceId> restorePrepared;
    const std::filesystem::path& target() const noexcept override { return finalPath; }
    const std::filesystem::path& staging() const noexcept override { return stagePath; }
    Result<void> validate() const;
};
Result<const Destination*> destination(const MaintenanceDestination&);
}  // namespace anof::infrastructure::system::maintenance
