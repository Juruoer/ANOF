#pragma once

#include <anof/application/ports/RequestIdentity.h>

#include <memory>

namespace anof::infrastructure::crypto {

// 构造时初始化 libsodium，并取得本次实例启动独立的 32 字节随机种子。
// 可报告的构造失败返回 InternalError；不能以固定种子或占位 ID 降级。
// 一个实例复用一个 provider，持有到 HTTP 清理及迟到任务观测结束。
// forExchange 的内部派生失败抛异常，不生成替代 ID；种子不持久化或记录。
Result<std::unique_ptr<RequestIdentity>> makeSodiumRequestIdentity();

}  // namespace anof::infrastructure::crypto
