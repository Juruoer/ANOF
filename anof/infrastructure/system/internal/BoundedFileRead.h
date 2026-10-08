#pragma once

#include <anof/core/Result.h>

#include <sys/types.h>

#include <cstddef>
#include <functional>
#include <string>

namespace anof::infrastructure::system::detail {

// 启动文件读取上限最多 64 KiB；fd 必须为已安全打开的普通文件，不接管所有权。
using FileReader = std::function<ssize_t(int, void*, std::size_t)>;
Result<std::string> readBoundedFile(int fd, std::size_t limit, ErrorCode oversized);
// 局部测试接缝：不安装全局回调，可控制短读、增长和错误。
Result<std::string> readBoundedFileWith(int fd, std::size_t limit, ErrorCode oversized, const FileReader& read);

}  // namespace anof::infrastructure::system::detail
