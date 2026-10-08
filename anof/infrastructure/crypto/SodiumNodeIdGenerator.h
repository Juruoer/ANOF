#pragma once

#include <anof/application/ports/NodeIdGenerator.h>

#include <memory>

namespace anof::infrastructure::crypto {

// 构造时完成 libsodium 初始化；可报告的构造失败返回 InternalError。
// next 使用独立安全随机源生成 UUID v4，不使用请求 ID 的种子或派生结果。
// next 的内部格式不变量失败返回 InternalError；随机源的库级致命终止不可恢复。
// 由元数据 worker 构造、使用并销毁，无数据库依赖。
Result<std::unique_ptr<NodeIdGenerator>> makeSodiumNodeIdGenerator();

}  // namespace anof::infrastructure::crypto
