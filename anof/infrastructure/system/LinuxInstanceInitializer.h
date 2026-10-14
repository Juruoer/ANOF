#pragma once

#include <anof/application/ports/InstanceInitialization.h>

#include <memory>

namespace anof::infrastructure::system {

// 构造不访问实例。工厂失败归于 Validate，实际操作报告各自阶段。
InitResult<std::unique_ptr<InstanceInitializer>> makeLinuxInstanceInitializer();

}  // namespace anof::infrastructure::system
