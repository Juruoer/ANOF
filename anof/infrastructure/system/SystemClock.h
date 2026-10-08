#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Result.h>

#include <memory>

namespace anof::infrastructure::system {

// 返回 UTC Unix epoch 微秒时间；不足一微秒的部分向下取整，含 epoch 前时间。
// 墙上时间允许回退。可报告的构造失败返回 InternalError。
// 由元数据 worker 构造、使用并销毁，不依赖 runtime 类型。
Result<std::unique_ptr<Clock>> makeSystemClock();

}  // namespace anof::infrastructure::system
