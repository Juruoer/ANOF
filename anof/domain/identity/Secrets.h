#pragma once

#include <anof/core/Result.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace anof {
namespace detail {
// 独立堆缓冲避免 std::string 的 SSO 移动留下本对象无法管理的副本。
// 只保证自身持有的字节清零，不承诺擦除调用者、寄存器或分页副本。
class SecretStorage {
public:
    explicit SecretStorage(std::string_view text);
    ~SecretStorage();
    SecretStorage(SecretStorage&& other) noexcept;
    SecretStorage& operator=(SecretStorage&& other) noexcept;
    SecretStorage(const SecretStorage&) = delete;
    SecretStorage& operator=(const SecretStorage&) = delete;
    std::string_view view() const noexcept { return size_ ? std::string_view(bytes_.get(), size_) : std::string_view{}; }

private:
    void clear() noexcept;
    std::unique_ptr<char[]> bytes_;
    std::size_t size_ = 0;
};
}  // namespace detail

class PasswordInput {
public:
    static Result<PasswordInput> parse(std::string_view text);
    PasswordInput(PasswordInput&&) noexcept = default;
    PasswordInput& operator=(PasswordInput&&) noexcept = default;
    std::string_view view() const noexcept { return value_.view(); }

private:
    explicit PasswordInput(std::string_view text) : value_(text) {}
    detail::SecretStorage value_;
};

class SecretToken {
public:
    static Result<SecretToken> session(std::string_view text);
    static Result<SecretToken> invite(std::string_view text);
    SecretToken(SecretToken&&) noexcept = default;
    SecretToken& operator=(SecretToken&&) noexcept = default;
    std::string_view view() const noexcept { return value_.view(); }

private:
    explicit SecretToken(std::string_view text) : value_(text) {}
    detail::SecretStorage value_;
};

class InviteTokenText {
public:
    static Result<InviteTokenText> parse(std::string_view text);
    InviteTokenText(InviteTokenText&&) noexcept = default;
    InviteTokenText& operator=(InviteTokenText&&) noexcept = default;
    std::string_view view() const noexcept { return value_.view(); }

private:
    explicit InviteTokenText(std::string_view text) : value_(text) {}
    detail::SecretStorage value_;
};

struct PasswordHashLimits {
    std::uint32_t maxMemoryKiB;
    std::uint32_t maxIterations;
    std::uint32_t maxParallelism;
};
struct PasswordHashParameters {
    std::uint32_t memoryKiB;
    std::uint32_t iterations;
    std::uint32_t parallelism;
};

class EncodedPasswordHash {
public:
    // 仅检查有界 Argon2id PHC 结构与参数；真正密码验证由 crypto 适配器负责。
    // limits 必须由已选定的密码预算提供，没有隐藏的无限上限。
    static Result<EncodedPasswordHash> parse(std::string_view text, PasswordHashLimits limits);
    EncodedPasswordHash(EncodedPasswordHash&&) noexcept = default;
    EncodedPasswordHash& operator=(EncodedPasswordHash&&) noexcept = default;
    std::string_view view() const noexcept { return value_.view(); }
    PasswordHashParameters parameters() const noexcept { return parameters_; }

private:
    EncodedPasswordHash(std::string_view text, PasswordHashParameters parameters) : value_(text), parameters_(parameters) {}
    detail::SecretStorage value_;
    PasswordHashParameters parameters_;
};
}  // namespace anof
