#pragma once
#include <anof/runtime/IdentityExecutor.h>

#include <filesystem>

namespace anof::runtime {
// 仅在对应 worker 调用；数据库及密码策略与实例初始化使用同一基线。
Result<MetadataWorkerResources> makeInstanceMetadataResources(const std::filesystem::path& database);
Result<PasswordWorkerResources> makeInstancePasswordResources();
}  // namespace anof::runtime
