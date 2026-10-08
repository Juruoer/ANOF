#include <anof/infrastructure/system/internal/UtcTimeConversion.h>

#include <limits>
#include <ratio>

namespace anof::infrastructure::system::detail {

UtcTime toUtcTime(std::chrono::system_clock::time_point time) noexcept {
    using Native = std::chrono::system_clock::duration;
    using Microseconds = std::chrono::microseconds;
    using Scale = std::ratio_divide<Native::period, Microseconds::period>;
    // 当前 Linux 时钟以有符号整数记录微秒或更细的整分刻度。
    // 限定可安全缩小的表示，避免移植到不兼容时钟后静默溢出。
    static_assert(std::numeric_limits<Native::rep>::is_integer && std::numeric_limits<Native::rep>::is_signed);
    static_assert(Scale::num == 1);
    static_assert(std::numeric_limits<Native::rep>::digits <= std::numeric_limits<Microseconds::rep>::digits);
    // floor 对 epoch 前不足一微秒的时间向负无穷取整，而不是向零截断。
    return std::chrono::floor<Microseconds>(time);
}

}  // namespace anof::infrastructure::system::detail
