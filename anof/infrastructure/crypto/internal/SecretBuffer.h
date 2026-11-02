#pragma once

#include <sodium.h>

#include <array>
#include <cstddef>

namespace anof::infrastructure::crypto::detail {
// 只管理适配器自己的临时副本；不承诺清除调用方或寄存器副本。
template <typename T, std::size_t Size>
struct SecretBuffer {
    std::array<T, Size> bytes{};
    SecretBuffer() = default;
    SecretBuffer(const SecretBuffer&) = delete;
    SecretBuffer& operator=(const SecretBuffer&) = delete;
    ~SecretBuffer() { sodium_memzero(bytes.data(), sizeof(bytes)); }
};
}  // namespace anof::infrastructure::crypto::detail
