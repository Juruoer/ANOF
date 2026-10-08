#pragma once

#include <anof/core/Id.h>

#include <cstdint>

namespace anof {

class RequestIdentity {
public:
    virtual ~RequestIdentity() = default;
    // 构造适配器时取得每次启动独立的 32 字节安全随机种子；失败不得进入 READY。
    // SHA-256(ASCII "ANOF/request-id/v1" + 种子 + connection/request 各 8 字节大端)，
    // 前 16 字节按非 nil UUID 文本表达；全零时令最后一字节为 1。不承诺 UUID version。
    // 相同交换返回相同 ID；无数据库、无每交换持久映射，不接受客户端关联 ID。
    // 不用于节点随机 UUID、认证或幂等；种子不能进入日志。
    virtual RequestId forExchange(std::uint64_t connection, std::uint64_t request) const = 0;
};

}  // namespace anof
