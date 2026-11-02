#include <anof/domain/identity/IdentityValues.h>
#include <anof/domain/identity/Secrets.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace anof {
namespace {
bool validUtf8(std::string_view text, bool rejectControls) {
    for (std::size_t i = 0; i < text.size();) {
        auto first = static_cast<unsigned char>(text[i++]);
        std::uint32_t point = first, minimum = 0;
        std::size_t extra = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            point = first & 31;
            extra = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            point = first & 15;
            extra = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            point = first & 7;
            extra = 3;
            minimum = 0x10000;
        } else if (first >= 0x80)
            return false;
        if (extra > text.size() - i) return false;
        for (std::size_t j = 0; j < extra; ++j) {
            auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            point = (point << 6) | (next & 63);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) || point == 0) return false;
        if (rejectControls && (point <= 31 || (point >= 127 && point <= 159))) return false;
    }
    return true;
}
bool base64(std::string_view text, bool url) {
    if (text.empty() || text.size() % 4 == 1) return false;
    int last = 0;
    for (char c : text) {
        if (c >= 'A' && c <= 'Z')
            last = c - 'A';
        else if (c >= 'a' && c <= 'z')
            last = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            last = c - '0' + 52;
        else if (c == (url ? '-' : '+'))
            last = 62;
        else if (c == (url ? '_' : '/'))
            last = 63;
        else
            return false;
    }
    return (text.size() % 4 != 2 || (last & 15) == 0) && (text.size() % 4 != 3 || (last & 3) == 0);
}
bool sessionText(std::string_view text) { return text.size() == 43 && base64(text, true); }
bool inviteText(std::string_view text) {
    return text.size() == 55 && text.substr(0, 12) == "anof_invite_" && sessionText(text.substr(12));
}
bool number(std::string_view text, std::uint32_t& result) {
    if (text.empty() || text.front() == '0') return false;
    result = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<std::uint32_t>(c - '0');
        if (result > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return false;
        result = result * 10 + digit;
    }
    return true;
}
}  // namespace

Result<LoginName> LoginName::parse(std::string_view text) {
    if (text.size() < 3 || text.size() > 32) return ErrorCode::InvalidArgument;
    std::string normalized(text);
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        auto& c = normalized[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c < 'a' || c > 'z') && (i == 0 || !((c >= '0' && c <= '9') || c == '_' || c == '-')))
            return ErrorCode::InvalidArgument;
    }
    return LoginName(std::move(normalized));
}
Result<DisplayName> DisplayName::parse(std::string_view text) {
    if (text.empty() || text.size() > 128 || !validUtf8(text, true)) return ErrorCode::InvalidArgument;
    return DisplayName(std::string(text));
}
Result<PasswordInput> PasswordInput::parse(std::string_view text) {
    if (text.size() < 12 || text.size() > 1024 || !validUtf8(text, false)) return ErrorCode::InvalidArgument;
    return PasswordInput(text);
}
Result<SecretToken> SecretToken::session(std::string_view text) {
    if (!sessionText(text)) return ErrorCode::InvalidArgument;
    return SecretToken(text);
}
Result<SecretToken> SecretToken::invite(std::string_view text) {
    if (!inviteText(text)) return ErrorCode::InvalidArgument;
    return SecretToken(text);
}
Result<InviteTokenText> InviteTokenText::parse(std::string_view text) {
    if (!inviteText(text)) return ErrorCode::InvalidArgument;
    return InviteTokenText(text);
}
Result<EncodedPasswordHash> EncodedPasswordHash::parse(std::string_view text, PasswordHashLimits limits) {
    constexpr std::string_view prefix = "$argon2id$v=19$m=";
    if (text.size() > 512 || text.substr(0, prefix.size()) != prefix) return ErrorCode::InvalidArgument;
    auto tail = text.substr(prefix.size());
    auto comma = tail.find(",t=");
    PasswordHashParameters p{};
    if (comma == std::string_view::npos || !number(tail.substr(0, comma), p.memoryKiB)) return ErrorCode::InvalidArgument;
    tail.remove_prefix(comma + 3);
    comma = tail.find(",p=");
    if (comma == std::string_view::npos || !number(tail.substr(0, comma), p.iterations)) return ErrorCode::InvalidArgument;
    tail.remove_prefix(comma + 3);
    auto split = tail.find('$');
    if (split == std::string_view::npos || !number(tail.substr(0, split), p.parallelism)) return ErrorCode::InvalidArgument;
    if (p.parallelism > limits.maxParallelism || p.iterations > limits.maxIterations || p.memoryKiB > limits.maxMemoryKiB ||
        static_cast<std::uint64_t>(p.memoryKiB) < 8ULL * p.parallelism) return ErrorCode::InvalidArgument;
    tail.remove_prefix(split + 1);
    split = tail.find('$');
    if (split == std::string_view::npos) return ErrorCode::InvalidArgument;
    auto salt = tail.substr(0, split), hash = tail.substr(split + 1);
    if (!base64(salt, false) || !base64(hash, false)) return ErrorCode::InvalidArgument;
    const auto saltBytes = salt.size() * 6 / 8, hashBytes = hash.size() * 6 / 8;
    if (saltBytes < 16 || saltBytes > 64 || hashBytes < 16 || hashBytes > 64) return ErrorCode::InvalidArgument;
    return EncodedPasswordHash(text, p);
}

detail::SecretStorage::SecretStorage(std::string_view text) : bytes_(text.empty() ? nullptr : std::make_unique<char[]>(text.size())), size_(text.size()) {
    if (size_) std::memcpy(bytes_.get(), text.data(), size_);
}
void detail::SecretStorage::clear() noexcept {
    volatile char* p = bytes_.get();
    for (std::size_t i = 0; i < size_; ++i) p[i] = 0;
    bytes_.reset();
    size_ = 0;
}
detail::SecretStorage::~SecretStorage() { clear(); }
detail::SecretStorage::SecretStorage(SecretStorage&& other) noexcept : bytes_(std::move(other.bytes_)), size_(std::exchange(other.size_, 0)) {}
detail::SecretStorage& detail::SecretStorage::operator=(SecretStorage&& other) noexcept {
    if (this != &other) {
        clear();
        bytes_ = std::move(other.bytes_);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}
}  // namespace anof
