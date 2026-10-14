#include <anof/runtime/AuthenticationLimiter.h>
#include <algorithm>
#include <stdexcept>

namespace anof::runtime {
AuthenticationLimiter::AuthenticationLimiter(std::size_t capacity) : capacity_(capacity) {
    if (capacity < 2 || capacity > 1024) throw std::invalid_argument("Invalid limiter capacity");
    entries_.reserve(capacity);
}
bool AuthenticationLimiter::allow(AuthenticationSource source, const LoginName& login, Time now) {
    if (updated_ && now < *updated_) now = *updated_;
    if (updated_) global_ = std::min(16.0, global_ + std::chrono::duration<double>(now - *updated_).count() * 0.5);
    updated_ = now;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) {
        return now - e.used >= std::chrono::minutes(10);
    }), entries_.end());
    std::size_t sourceIndex = entries_.size(), pairIndex = entries_.size();
    for (std::size_t i = 0; i < entries_.size(); ++i) if (entries_[i].source == source) {
        if (!entries_[i].login) sourceIndex = i;
        else if (*entries_[i].login == login.value()) pairIndex = i;
    }
    const auto absent = entries_.size();
    const auto missing = static_cast<std::size_t>(sourceIndex == absent) + static_cast<std::size_t>(pairIndex == absent);
    if (entries_.size() + missing > capacity_) return false;
    // 分配登录名可能抛出，调用方将其作为同步拒绝；尚未接纳工作或扣除 token。
    std::optional<std::string> owned;
    if (pairIndex == absent) owned = login.value();
    if (sourceIndex == absent) { sourceIndex = entries_.size(); entries_.push_back({source, std::nullopt, 5, now, now}); }
    if (pairIndex == absent) { pairIndex = entries_.size(); entries_.push_back({source, std::move(owned), 3, now, now}); }
    auto refill = [&](Entry& e, double burst, double rate) {
        e.tokens = std::min(burst, e.tokens + std::chrono::duration<double>(now - e.updated).count() * rate);
        e.updated = now; e.used = now;
    };
    auto& address = entries_[sourceIndex]; auto& pair = entries_[pairIndex];
    refill(address, 5, 10.0 / 60); refill(pair, 3, 5.0 / 60);
    if (global_ < 1 || address.tokens < 1 || pair.tokens < 1) return false;
    --global_; --address.tokens; --pair.tokens;
    return true;
}
}  // namespace anof::runtime
