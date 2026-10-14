#pragma once

#include <anof/application/ports/MetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteConnection.h>

namespace anof::infrastructure::sqlite::detail {
// SQLite 内部连接装配。业务调用方只能使用经过引导校验的公开打开工厂。
class MetadataStoreImpl final : public MetadataStore {
public:
    MetadataStoreImpl(std::unique_ptr<SqliteConnection> connection, PasswordHashLimits limits)
        : connection_(std::move(connection)), limits_(limits) {}
    Result<std::unique_ptr<MetadataTransaction>> beginWrite() override;
    Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() override;
private:
    std::unique_ptr<SqliteConnection> connection_;
    PasswordHashLimits limits_;
    bool active_ = false;
};
}  // namespace anof::infrastructure::sqlite::detail
