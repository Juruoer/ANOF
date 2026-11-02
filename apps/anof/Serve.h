#pragma once
#include <filesystem>

namespace anof::app {
// 前台运行已存在实例；不创建实例、账户或凭据。返回值见入口参考文档。
int serve(const std::filesystem::path& root);
}  // namespace anof::app
