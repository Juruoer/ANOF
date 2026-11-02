#pragma once
#include <anof/application/ports/MaintenanceFileStore.h>
#include <anof/application/ports/MetadataInspection.h>
#include <anof/application/ports/MetadataRepair.h>
#include <anof/application/ports/MetadataSnapshot.h>
namespace anof::testing {
// 后端工厂提供真实私有临时实例；契约用例只使用类型化端口，不执行SQL。
class TestMaintenanceBackend {
public:
    virtual ~TestMaintenanceBackend() = default;
    virtual const std::filesystem::path& root() const = 0;
    virtual std::filesystem::path work() const = 0;
    virtual const InstanceLease& lease() const = 0;
    virtual InstanceId instance() const = 0;
    virtual MetadataInspection& inspection() = 0;
    virtual MetadataRepair& repair() = 0;
    virtual MetadataSnapshot& snapshot() = 0;
    virtual OfflineInstanceAccess& access() = 0;
    virtual MaintenanceFileStore& files() = 0;
};
std::unique_ptr<TestMaintenanceBackend> makeTestMaintenanceBackend();
}  // namespace anof::testing
