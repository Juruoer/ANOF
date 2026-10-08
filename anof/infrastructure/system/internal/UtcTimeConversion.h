#pragma once

#include <anof/core/Clock.h>

namespace anof::infrastructure::system::detail {

// 纯转换接缝；不调整系统时间，不包含全局时钟替身。
UtcTime toUtcTime(std::chrono::system_clock::time_point time) noexcept;

}  // namespace anof::infrastructure::system::detail
