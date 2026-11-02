#pragma once

#include <anof/core/Result.h>

#include <csignal>
#include <memory>
#include <thread>

namespace anof::app {

// Linux 前台进程的 SIGINT/SIGTERM 桥接。必须在创建任何 worker 前由主线程构造，
// worker 全部退出后由同一线程销毁。新线程继承屏蔽集；不安装异步 signal handler。
class ShutdownSignals {
public:
    static Result<std::unique_ptr<ShutdownSignals>> create();
    ~ShutdownSignals();
    ShutdownSignals(const ShutdownSignals&) = delete;
    ShutdownSignals& operator=(const ShutdownSignals&) = delete;

    // 主 loop 的定时器调用；有界、非阻塞，true 一旦出现就保持到析构。
    // 收到 true 后在普通 loop 上请求服务停止，不从信号上下文调用 muduox。
    Result<bool> poll();

private:
    ShutdownSignals() = default;
    int fd_ = -1;
    bool ownsRegistration_ = false;
    bool maskInstalled_ = false;
    bool requested_ = false;
    sigset_t original_{};
    std::thread::id owner_ = std::this_thread::get_id();
};

}  // namespace anof::app
