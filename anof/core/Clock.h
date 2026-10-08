#pragma once

#include <chrono>

namespace anof {

// 持久化和测试统一为 Unix epoch 起算的 UTC 微秒；HTTP 编解码由接入层负责。
using UtcTime = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

class Clock {
public:
    virtual ~Clock() = default;
    virtual UtcTime now() const = 0;
};

}  // namespace anof
