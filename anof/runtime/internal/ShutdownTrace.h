#pragma once

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace anof::runtime::internal {
inline bool shutdownTraceEnabled() noexcept {
    static const bool enabled = std::getenv("ANOF_SHUTDOWN_TRACE") != nullptr;
    return enabled;
}
// 按需诊断：只记录固定事件、对象标识和状态，不读取凭据或业务数据。
inline void shutdownTrace(const char* component, const char* event, const void* object = nullptr,
                          long a = -1, long b = -1, long c = -1) noexcept {
    if (!shutdownTraceEnabled()) return;
    const double mono = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double wall = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(stderr, "SHUTDOWN_TRACE pid=%ld mono=%.9f wall=%.9f component=%s event=%s object=%p a=%ld b=%ld c=%ld\n",
                 static_cast<long>(::getpid()), mono, wall, component, event, object, a, b, c);
}
}  // namespace anof::runtime::internal
