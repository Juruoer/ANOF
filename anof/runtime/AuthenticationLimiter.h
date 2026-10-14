#pragma once
#include <anof/domain/identity/AuthenticationSource.h>
#include <anof/domain/identity/IdentityValues.h>
#include <chrono>
#include <optional>
#include <vector>

namespace anof::runtime {
// 所属 loop 使用；独立于 HTTP 与墙上时间。表满时拒绝，不逐出仍活跃条目以重置预算。
class AuthenticationLimiter {
public:
    using Time = std::chrono::steady_clock::time_point;
    explicit AuthenticationLimiter(std::size_t capacity = 1024);
    bool allow(AuthenticationSource source, const LoginName& login, Time now);
private:
    struct Entry {
        AuthenticationSource source;
        std::optional<std::string> login;
        double tokens;
        Time updated, used;
    };
    std::size_t capacity_;
    std::vector<Entry> entries_;
    double global_ = 16;
    std::optional<Time> updated_;
};
}  // namespace anof::runtime
