#include <anof/domain/transfer/TransferValues.h>
#include <algorithm>

namespace anof {
Result<Sha256> Sha256::parse(std::string_view text) {
    if (text.size() != 64) return ErrorCode::InvalidArgument;
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        int a = hex(text[i * 2]), b = hex(text[i * 2 + 1]);
        if (a < 0 || b < 0) return ErrorCode::InvalidArgument;
        bytes[i] = static_cast<std::uint8_t>(a * 16 + b);
    }
    return Sha256(bytes);
}
std::string Sha256::toString() const {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(64, '0');
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        text[i * 2] = digits[bytes_[i] >> 4];
        text[i * 2 + 1] = digits[bytes_[i] & 15];
    }
    return text;
}
Result<ByteCount> parseTransferDecimal(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return ErrorCode::InvalidArgument;
    std::int64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return ErrorCode::InvalidArgument;
        int digit = c - '0';
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) return ErrorCode::InvalidArgument;
        value = value * 10 + digit;
    }
    return ByteCount::from(value);
}
Result<UploadOffset> checkedUploadEnd(FileLength length, UploadOffset offset, ByteCount count) {
    if (offset.value() > length.value()) return ErrorCode::IntegrityError;
    if (count.value() > length.value() - offset.value()) return ErrorCode::UploadLengthMismatch;
    return offset.add(count.value());
}
Result<ByteCount> diskSafetyMargin(ByteCount volume, const DiskSafetyPolicy& policy) {
    // 分解商余数，避免 volume * percent 溢出；向上取整不低估余量。
    const auto p = policy.percent.value();
    const auto n = volume.value();
    return ByteCount::from(std::max(policy.minimum.value(), (n / 100) * p + ((n % 100) * p + 99) / 100));
}
Result<void> requireDiskCapacity(ByteCount free, ByteCount unwritten, FileLength incoming, ByteCount margin) {
    auto remaining = free.value();
    for (auto demand : { unwritten.value(), incoming.value(), margin.value() }) {
        if (demand > remaining) return ErrorCode::StorageFull;
        remaining -= demand;
    }
    return {};
}
}  // namespace anof
