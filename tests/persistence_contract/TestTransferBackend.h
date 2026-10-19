#pragma once
#include <tests/persistence_contract/TestMetadataBackend.h>
#include <anof/application/ports/TransferMetadataStore.h>

namespace anof::testing {
struct TransferSeed {
    MetadataSeed metadata;
    StorageSnapshot storage;
    std::vector<Upload> uploads;
};
// 真实临时数据库工厂；不以 mock 宣称事务/约束通过。
// reset 仅在无连接时使用；backend 最后析构并清理其私有目录。
class TestTransferBackend {
public:
    virtual ~TestTransferBackend() = default;
    virtual Result<void> reset(const TransferSeed&) = 0;
    virtual Result<std::unique_ptr<TransferMetadataStore>> connect() = 0;
    virtual Result<std::vector<TransferEvent>> readTransferAudit() = 0;
};
std::unique_ptr<TestTransferBackend> makeTestTransferBackend();
}  // namespace anof::testing
