#pragma once
#include <tests/persistence_contract/TestTransferBackend.h>

#include <filesystem>
namespace anof::infrastructure::sqlite::testing {
class SqliteTestTransferBackend final : public anof::testing::TestTransferBackend {
public:
    SqliteTestTransferBackend();
    ~SqliteTestTransferBackend() override;
    Result<void> reset(const anof::testing::TransferSeed&) override;
    Result<std::unique_ptr<TransferMetadataStore>> connect() override;
    Result<std::vector<TransferEvent>> readTransferAudit() override;
    std::filesystem::path databasePath() const { return root_ / "db"; }

private:
    std::filesystem::path root_;
};
}  // namespace anof::infrastructure::sqlite::testing
