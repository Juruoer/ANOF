#pragma once

#include <anof/core/Quantity.h>
#include <anof/domain/identity/SessionDigest.h>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace anof {
using PasswordVersion = Quantity<struct PasswordVersionTag, 1>;
using UserLimit = Quantity<struct UserLimitTag, 1, 10000>;
using UserCount = Quantity<struct UserCountTag>;
using SessionCount = Quantity<struct SessionCountTag>;
using InviteUseCount = Quantity<struct InviteUseCountTag, 0, 10000>;
using InviteUseLimit = Quantity<struct InviteUseLimitTag, 1, 10000>;

enum class CredentialMode { Bearer };
struct SessionCredential {
    SessionDigest digest;
    CredentialMode mode;
};

class InviteDigest {
public:
    using Bytes = std::array<std::uint8_t, 32>;
    explicit InviteDigest(Bytes bytes) : bytes_(bytes) {}
    const Bytes& bytes() const noexcept { return bytes_; }
    friend bool operator==(const InviteDigest& a, const InviteDigest& b) noexcept { return a.bytes_ == b.bytes_; }
    friend bool operator!=(const InviteDigest& a, const InviteDigest& b) noexcept { return !(a == b); }
private:
    Bytes bytes_;
};

class LoginName {
public:
    static Result<LoginName> parse(std::string_view text);
    const std::string& value() const noexcept { return value_; }
    friend bool operator==(const LoginName& a, const LoginName& b) { return a.value_ == b.value_; }
private:
    explicit LoginName(std::string text) : value_(std::move(text)) {}
    std::string value_;
};

class DisplayName {
public:
    static Result<DisplayName> parse(std::string_view text);
    const std::string& value() const noexcept { return value_; }
private:
    explicit DisplayName(std::string text) : value_(std::move(text)) {}
    std::string value_;
};
}  // namespace anof
