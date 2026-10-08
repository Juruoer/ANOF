#pragma once

#include <anof/core/Result.h>
#include <anof/domain/identity/SessionDigest.h>

#include <string_view>

namespace anof {

class SessionTokenCodec {
public:
    virtual ~SessionTokenCodec() = default;
    // 只接收凭据文本，不含 Bearer scheme；不保留借用的输入。
    // 必须是 32 字节随机 token 的规范、无 padding base64url（43 字符，校验末尾位）。
    // SHA-256 输入为解码后的 32 字节，绝非编码文本；非法输入为 Unauthenticated，
    // 内部失败为 InternalError。禁止记录 token/摘要；不得执行数据库或密码哈希。
    virtual Result<SessionDigest> digestBearer(std::string_view token) const = 0;
};

}  // namespace anof
