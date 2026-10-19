#pragma once

#include <anof/core/Quantity.h>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace anof {
inline constexpr std::int64_t kMaxFileBytes = 1099511627776;
inline constexpr std::size_t kTransferBlockBytes = 64 * 1024;
inline constexpr std::int64_t kMaxPatchBytes = 8 * 1024 * 1024;
using FileLength = Quantity<struct FileLengthTag, 0, kMaxFileBytes>;
using UploadOffset = Quantity<struct UploadOffsetTag>;
using UploadGeneration = Quantity<struct UploadGenerationTag, 1>;
using SafetyPercent = Quantity<struct SafetyPercentTag, 0, 100>;

class Sha256 {
public:
    explicit Sha256(std::array<std::uint8_t, 32> bytes) : bytes_(bytes) {}
    static Result<Sha256> parse(std::string_view lowercaseHex);
    std::string toString() const;
    const std::array<std::uint8_t, 32>& bytes() const noexcept { return bytes_; }
    friend bool operator==(const Sha256& a, const Sha256& b) { return a.bytes_ == b.bytes_; }
    friend bool operator!=(const Sha256& a, const Sha256& b) { return !(a == b); }
private:
    std::array<std::uint8_t, 32> bytes_;
};

// wire 的规范十进制：无符号、无空白、无前导零。HTTP Content-Length 由 transport 单独解析。
Result<ByteCount> parseTransferDecimal(std::string_view);
Result<UploadOffset> checkedUploadEnd(FileLength, UploadOffset, ByteCount count);
// 适配器用于 size_t/off_t 等目标类型；领域层不包含 POSIX 类型。
template <typename Integer>
Result<Integer> checkedTransferInteger(ByteCount value) {
    static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>);
    if (static_cast<std::uintmax_t>(value.value()) >
        static_cast<std::uintmax_t>(std::numeric_limits<Integer>::max())) return ErrorCode::InvalidArgument;
    return static_cast<Integer>(value.value());
}
struct DiskSafetyPolicy {
    ByteCount minimum = ByteCount::from(1073741824).value();
    SafetyPercent percent = SafetyPercent::from(5).value();
};
Result<ByteCount> diskSafetyMargin(ByteCount volumeBytes, const DiskSafetyPolicy&);
Result<void> requireDiskCapacity(ByteCount free, ByteCount unwritten, FileLength incoming, ByteCount margin);
}  // namespace anof
