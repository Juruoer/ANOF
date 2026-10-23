#pragma once
#include <tests/persistence_contract/TestDriveBackend.h>

#include <filesystem>
namespace anof::infrastructure::sqlite::testing {
class SqliteTestDriveBackend final : public anof::testing::TestDriveBackend {
public:
    SqliteTestDriveBackend();
    ~SqliteTestDriveBackend() override;
    Result<void> reset(const anof::testing::DriveSeed&) override;
    Result<std::unique_ptr<DriveMetadataStore>> connect() override;
    Result<std::vector<DriveEvent>> readDriveAudit() override;
    std::filesystem::path databasePath() const { return root_ / "db"; }

private:
    std::filesystem::path root_;
};
}  // namespace anof::infrastructure::sqlite::testing
