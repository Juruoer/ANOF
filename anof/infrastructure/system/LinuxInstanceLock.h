#pragma once

#include <anof/application/ports/InstancePreparation.h>

#include <memory>

namespace anof::infrastructure::system {

// 工厂不访问实例路径；可报告的构造失败返回 InternalError。
// acquire 只接纳已有绝对实例根，成功返回持有目录及锁描述符的非空 lease。
// 非阻塞锁竞争为 Conflict；lease 析构释放，不删除或替换锁文件。
Result<std::unique_ptr<InstanceLockProvider>> makeLinuxInstanceLockProvider();

}  // namespace anof::infrastructure::system
