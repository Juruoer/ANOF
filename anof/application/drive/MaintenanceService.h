#pragma once
#include <anof/application/ports/DriveMetadataStore.h>
namespace anof {
// 仅受信任后台协调器使用，无用户授权或磁盘入口。一次调用最多一个短事务。
class MaintenanceService {
public:
    MaintenanceService(DriveMetadataStore& store, Clock& clock) : store_(store), clock_(clock) {}
    Result<void> expire(TrashCondition, RequestId);
    Result<BlobRecord> mark(BlobId, Revision, RequestId);
    Result<void> finalize(BlobId, Revision, RequestId);

private:
    DriveMetadataStore& store_;
    Clock& clock_;
};
}  // namespace anof
