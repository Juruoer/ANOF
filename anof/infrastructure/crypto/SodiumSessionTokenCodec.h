#pragma once

#include <anof/application/ports/SessionTokenCodec.h>

#include <memory>

namespace anof::infrastructure::crypto {

// 构造时完成 libsodium 初始化；首次及已经初始化均可成功。
// 可报告的构造失败返回 InternalError，不返回空的成功值。
// 调用方持有到 HTTP 清理及迟到任务观测结束；不保留借用的 token。
Result<std::unique_ptr<SessionTokenCodec>> makeSodiumSessionTokenCodec();

}  // namespace anof::infrastructure::crypto
