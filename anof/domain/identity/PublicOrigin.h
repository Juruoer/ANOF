#pragma once
#include <anof/core/Result.h>

#include <string>
#include <string_view>
#include <utility>

namespace anof {
// 固定HTTPS origin；值不携带HTTP请求、JSON或代理库类型。
class PublicOrigin {
public:
    static Result<PublicOrigin> parse(std::string_view);
    const std::string& value() const noexcept { return value_; }

private:
    explicit PublicOrigin(std::string value) : value_(std::move(value)) {}
    std::string value_;
};
bool isTrustedLoopbackProxy(std::string_view) noexcept;
}  // namespace anof
