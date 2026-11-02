#pragma once

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace anof::test {
// 仅记录已有检查的结果；不能为诊断再次调用可能读取 socket 的 predicate。
class WaitTrace {
public:
    WaitTrace(const char* file, int line)
        : enabled_(std::getenv("ANOF_WAIT_TRACE") != nullptr), file_(file), line_(line) {}

    void event(const char* name, bool observedDone = false) const {
        if (!enabled_) return;
        const auto mono = std::chrono::steady_clock::now();
        const auto wall = std::chrono::system_clock::now();
        const double elapsed = std::chrono::duration<double>(mono - monoStart_).count();
        const double wallElapsed = std::chrono::duration<double>(wall - wallStart_).count();
        std::fprintf(stderr,
                     "WAIT_TRACE pid=%ld site=%s:%d event=%s mono=%.9f elapsed=%.9f "
                     "wall_elapsed=%.9f offset_delta=%.9f observed_done=%d\n",
                     static_cast<long>(::getpid()), file_, line_, name,
                     std::chrono::duration<double>(mono.time_since_epoch()).count(),
                     elapsed, wallElapsed, wallElapsed - elapsed, observedDone ? 1 : 0);
    }

private:
    bool enabled_;
    const char* file_;
    int line_;
    std::chrono::steady_clock::time_point monoStart_ = std::chrono::steady_clock::now();
    std::chrono::system_clock::time_point wallStart_ = std::chrono::system_clock::now();
};
}  // namespace anof::test
