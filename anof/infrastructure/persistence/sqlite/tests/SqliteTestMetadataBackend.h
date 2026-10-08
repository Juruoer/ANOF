#pragma once

#include <tests/persistence_contract/TestMetadataBackend.h>

#include <filesystem>

namespace anof::infrastructure::sqlite::testing {

// 仅测试使用；reset 时必须没有连接，read* 时必须没有活动写事务。
class SqliteTestMetadataBackend final : public anof::testing::TestMetadataBackend {
public:
    SqliteTestMetadataBackend();
    ~SqliteTestMetadataBackend() override;
    SqliteTestMetadataBackend(const SqliteTestMetadataBackend&) = delete;
    SqliteTestMetadataBackend& operator=(const SqliteTestMetadataBackend&) = delete;
    Result<void> reset(const anof::testing::MetadataSeed& seed) override;
    Result<std::unique_ptr<MetadataStore>> connect() override;
    Result<std::optional<anof::testing::NodeSeed>> readNode(NodeId id) override;
    Result<QuotaState> readQuota(const QuotaScope& scope) override;
    Result<std::vector<DirectoryCreatedEvent>> readAudit() override;
    // SQLite 专项测试在独占 fixture 时通过该路径注入故障；不是生产工厂入口。
    std::filesystem::path databasePath() const;
private:
    std::filesystem::path directory_;
};

}  // namespace anof::infrastructure::sqlite::testing
