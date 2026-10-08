#pragma once

#include <anof/application/ports/InstancePreparation.h>

#include <memory>

namespace anof::infrastructure::sqlite {

// 工厂不打开数据库；可报告的构造失败返回 InternalError。
// verify 仅接受 Linux 实例锁提供者的活动 lease，检查已有格式和元数据。
// 不创建实例、不升级 schema；返回前关闭全部维护连接和临时资源。
Result<std::unique_ptr<InstanceMaintenance>> makeSqliteInstanceMaintenance();

}  // namespace anof::infrastructure::sqlite
