#pragma once

#include <anof/core/Result.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace anof {

// ID 只接受非 nil UUID 的规范分组形式；大小写输入统一输出为小写。
// 不在这里生成随机数或假定外部 UUID 的版本。
template <typename Tag>
class Id {
public:
    static Result<Id> parse(std::string_view text) {
        if (text.size() != 36) return ErrorCode::InvalidArgument;
        std::array<std::uint8_t, 16> bytes{};
        std::size_t digit = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (text[i] != '-') return ErrorCode::InvalidArgument;
                continue;
            }
            const char c = text[i];
            const int hex = c >= '0' && c <= '9' ? c - '0' :
                            c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                            c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (hex < 0) return ErrorCode::InvalidArgument;
            auto& byte = bytes[digit / 2];
            byte = static_cast<std::uint8_t>(byte * 16 + hex);
            ++digit;
        }
        if (std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0; })) {
            return ErrorCode::InvalidArgument;
        }
        return Id(bytes);
    }

    std::string toString() const {
        constexpr char hex[] = "0123456789abcdef";
        std::string text;
        text.reserve(36);
        for (std::size_t i = 0; i < bytes_.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
            text += hex[bytes_[i] >> 4];
            text += hex[bytes_[i] & 15];
        }
        return text;
    }

    friend bool operator==(const Id& a, const Id& b) noexcept { return a.bytes_ == b.bytes_; }
    friend bool operator!=(const Id& a, const Id& b) noexcept { return !(a == b); }
    friend bool operator<(const Id& a, const Id& b) noexcept { return a.bytes_ < b.bytes_; }

private:
    explicit Id(std::array<std::uint8_t, 16> bytes) : bytes_(bytes) {}
    std::array<std::uint8_t, 16> bytes_;
};

using UserId = Id<struct UserIdTag>;
using SpaceId = Id<struct SpaceIdTag>;
using NodeId = Id<struct NodeIdTag>;
using BlobId = Id<struct BlobIdTag>;
// 当前 request ID 由可信入口生成 UUID；不接受 JSON 中指定的 actor 或 request ID。
using RequestId = Id<struct RequestIdTag>;

}  // namespace anof
