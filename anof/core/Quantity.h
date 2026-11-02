#pragma once

#include <anof/core/Result.h>

#include <cstdint>
#include <limits>

namespace anof {

// 统一使用 SQLite 可表示的非负整数范围，但不暴露数据库类型。
template <typename Tag, std::int64_t Minimum = 0, std::int64_t Maximum = std::numeric_limits<std::int64_t>::max()>
class Quantity {
    static_assert(0 <= Minimum && Minimum <= Maximum);

public:
    Quantity() = default;
    static Result<Quantity> from(std::int64_t value) {
        if (value < Minimum || value > Maximum) return ErrorCode::InvalidArgument;
        return Quantity(value);
    }
    std::int64_t value() const noexcept { return value_; }
    Result<Quantity> add(std::int64_t delta) const {
        if (delta < 0 || value_ > Maximum - delta) {
            return ErrorCode::IntegrityError;
        }
        return Quantity(value_ + delta);
    }
    friend bool operator==(Quantity a, Quantity b) noexcept { return a.value_ == b.value_; }
    friend bool operator!=(Quantity a, Quantity b) noexcept { return !(a == b); }

private:
    explicit Quantity(std::int64_t value) : value_(value) {}
    std::int64_t value_ = Minimum;
};

using ByteCount = Quantity<struct ByteCountTag>;
using NodeCount = Quantity<struct NodeCountTag>;
using Revision = Quantity<struct RevisionTag, 1>;
using AuthEpoch = Quantity<struct AuthEpochTag>;

}  // namespace anof
