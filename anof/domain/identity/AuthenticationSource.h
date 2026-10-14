#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace anof {
// 可信入口从 socket peer 地址构造；不接受 JSON、X-Forwarded-For 或客户端自报来源。
// IPv4 统一为 IPv4-mapped IPv6，避免相同来源使用两种表示绕过限流。
class AuthenticationSource {
public:
    static AuthenticationSource ipv4(std::array<std::uint8_t, 4> bytes) {
        std::array<std::uint8_t, 16> value{};
        value[10] = 255; value[11] = 255;
        for (std::size_t i = 0; i < bytes.size(); ++i) value[i + 12] = bytes[i];
        return AuthenticationSource(value);
    }
    static AuthenticationSource ipv6(std::array<std::uint8_t, 16> bytes) { return AuthenticationSource(bytes); }
    friend bool operator==(const AuthenticationSource& a, const AuthenticationSource& b) { return a.bytes_ == b.bytes_; }
private:
    explicit AuthenticationSource(std::array<std::uint8_t, 16> bytes) : bytes_(bytes) {}
    std::array<std::uint8_t, 16> bytes_;
};
}  // namespace anof
