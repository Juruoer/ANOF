#pragma once
#include <tests/persistence_contract/TestTransferBackend.h>
#include <anof/application/ports/DriveMetadataStore.h>
namespace anof::testing {
struct DriveSeed {
    TransferSeed transfer;
    std::vector<BlobRecord> blobRecords;
    std::vector<TrashEntry> trashEntries;
    std::vector<std::pair<NodeId, TrashId>> memberships;
    NodeAccounting nodeAccounting;
};
// infrastructure第四步提供真实后端。reset拒绝不一致种子，不将mock计入验收。
class TestDriveBackend {
public:
    virtual ~TestDriveBackend() = default;
    virtual Result<void> reset(const DriveSeed&) = 0;
    virtual Result<std::unique_ptr<DriveMetadataStore>> connect() = 0;
    virtual Result<std::vector<DriveEvent>> readDriveAudit() = 0;
};
std::unique_ptr<TestDriveBackend> makeTestDriveBackend();
}  // namespace anof::testing
