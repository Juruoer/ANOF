#include <muduox/net/EventLoop.h>

#include <chrono>
#include <cstdio>

// 独立于 HTTP/worker 的人工诊断程序，由外部 runner 提供单调时钟进程超时。
int main() {
    muduox::net::EventLoop loop;
    const auto start = std::chrono::steady_clock::now();
    const auto wallStart = std::chrono::system_clock::now();
    auto previous = start;
    unsigned ticks = 0;
    double maxGap = 0;
    const auto poll = loop.runEvery(0.001, [&] {
        const auto now = std::chrono::steady_clock::now();
        const double gap = std::chrono::duration<double>(now - previous).count();
        if (gap > maxGap) maxGap = gap;
        previous = now;
        ++ticks;
    });
    loop.runAfter(3, [&] {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        const double wallElapsed = std::chrono::duration<double>(std::chrono::system_clock::now() - wallStart).count();
        std::printf("TIMER_PROBE mono=%.9f target=3 elapsed=%.9f wall_elapsed=%.9f "
                    "offset_delta=%.9f ticks=%u max_tick_gap=%.9f\n",
                    std::chrono::duration<double>(now.time_since_epoch()).count(),
                    elapsed, wallElapsed, wallElapsed - elapsed, ticks, maxGap);
        loop.quit();
    });
    loop.loop();
    loop.cancel(poll);
}
