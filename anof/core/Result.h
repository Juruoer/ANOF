#pragma once

#include <anof/core/Error.h>

#include <utility>
#include <variant>

namespace anof {

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : state_(std::move(value)) {}
    Result(Error error) : state_(error) {}
    Result(ErrorCode code) : state_(Error{ code }) {}

    bool hasValue() const noexcept { return std::holds_alternative<T>(state_); }
    explicit operator bool() const noexcept { return hasValue(); }
    T& value() & { return std::get<T>(state_); }
    const T& value() const& { return std::get<T>(state_); }
    T&& value() && { return std::get<T>(std::move(state_)); }
    const Error& error() const { return std::get<Error>(state_); }

private:
    std::variant<T, Error> state_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(error), success_(false) {}
    Result(ErrorCode code) : Result(Error{ code }) {}

    bool hasValue() const noexcept { return success_; }
    explicit operator bool() const noexcept { return hasValue(); }
    void value() const {
        if (!success_) throw std::bad_variant_access{};
    }
    const Error& error() const {
        if (success_) throw std::bad_variant_access{};
        return error_;
    }

private:
    // void 成功分支无需联合存储；始终初始化所有字段，避免优化复制时的未初始化诊断。
    Error error_{ ErrorCode::InternalError };
    bool success_ = true;
};

}  // namespace anof
