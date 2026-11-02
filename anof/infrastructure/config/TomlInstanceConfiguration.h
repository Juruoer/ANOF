#pragma once

#include <anof/application/ports/InstancePreparation.h>

#include <memory>

namespace anof::infrastructure::config {

// 恢复用显式完整配置；沿用安全目录/文件权限与同一TOML解析器，不写回默认值。
Result<InstanceConfiguration> loadExplicitInstanceConfiguration(const std::filesystem::path&);

// 工厂不读文件；可报告的构造失败返回 InternalError。
// load 仅接受 Linux 实例锁提供者的活动 lease，经目录描述符读取固定配置。
// 不保留 lease 引用；返回自有值，非法配置为 InvalidArgument。
Result<std::unique_ptr<InstanceConfigurationLoader>> makeTomlInstanceConfigurationLoader();

}  // namespace anof::infrastructure::config
