#pragma once

#include <anof/core/Id.h>
#include <array>
#include <string_view>

namespace anof::infrastructure::crypto::detail {
template <typename IdType>
Result<IdType> uuidFromRandomBytes(std::array<unsigned char, 16> bytes) {
    // UUID v4：第 6 字节高四位为 0100，第 8 字节高两位为 10。
    // 其余 122 位保持随机源输出；设置这些位也保证结果非 nil。
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    constexpr char hex[] = "0123456789abcdef";
    std::array<char, 36> text{};
    std::size_t position = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text[position++] = '-';
        text[position++] = hex[bytes[i] >> 4];
        text[position++] = hex[bytes[i] & 15];
    }
    auto id = IdType::parse(std::string_view(text.data(), text.size()));
    if (!id) return ErrorCode::InternalError;
    return id.value();
}

}  // namespace anof::infrastructure::crypto::detail
