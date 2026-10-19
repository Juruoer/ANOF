#pragma once

#include <algorithm>
#include <chrono>

namespace anof {

// 持久化和测试统一为 Unix epoch 起算的 UTC 微秒；HTTP 编解码由接入层负责。
using UtcTime = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

// 记录内事件时间：允许相等，不加一微秒；不用于认证、到期判断或全局时钟修正。
// 调用者从同一事务的持久记录取得created/previous，跨重启仍保持记录内顺序。
inline UtcTime nondecreasingEventTime(UtcTime observed, UtcTime created, UtcTime previous) noexcept {
    return std::max(observed, std::max(created, previous));
}

class Clock {
public:
    virtual ~Clock() = default;
    virtual UtcTime now() const = 0;
};

}  // namespace anof
