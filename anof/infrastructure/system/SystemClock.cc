#include <anof/infrastructure/system/SystemClock.h>
#include <anof/infrastructure/system/internal/UtcTimeConversion.h>

#include <new>

namespace anof::infrastructure::system {
namespace {

class SystemClock final : public Clock {
public:
    UtcTime now() const override {
        return detail::toUtcTime(std::chrono::system_clock::now());
    }
};

}  // namespace

Result<std::unique_ptr<Clock>> makeSystemClock() {
    try {
        return std::unique_ptr<Clock>(new SystemClock);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}

}  // namespace anof::infrastructure::system
