#pragma once

#include <array>
#include <cstdint>

namespace anof {

class SessionDigest {
public:
    using Bytes = std::array<std::uint8_t, 32>;
    explicit SessionDigest(Bytes bytes) : bytes_(bytes) {}
    const Bytes& bytes() const noexcept { return bytes_; }
    friend bool operator==(const SessionDigest& a, const SessionDigest& b) noexcept { return a.bytes_ == b.bytes_; }
    friend bool operator!=(const SessionDigest& a, const SessionDigest& b) noexcept { return !(a == b); }

private:
    Bytes bytes_;
};

}  // namespace anof
