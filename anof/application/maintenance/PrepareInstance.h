#pragma once

#include <anof/application/ports/InstancePreparation.h>

namespace anof {

struct PreparedInstance {
    std::unique_ptr<InstanceLease> lease;
    InstanceConfiguration configuration;
};

// 当前服务仅支持显式启用的本机 HTTP；0 端口让内核选取动态端口。
Result<void> validateInstanceConfiguration(const InstanceConfiguration& configuration);

// 先锁、再读取配置、最后校验元数据；任何失败均释放本次获得的锁。
// 在启动 HTTP／worker 前调用，不在服务请求回调中执行阻塞维护操作。
Result<PreparedInstance> prepareInstance(const std::filesystem::path& root, InstanceLockProvider& locks,
                                         InstanceConfigurationLoader& configuration, InstanceMaintenance& maintenance);

}  // namespace anof
