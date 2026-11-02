#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace muduox::net {
class EventLoop;
}

namespace anof::runtime {

// 只负责合并唤醒；结果和预留容量由 MetadataExecutor 持有。
class CompletionPump {
public:
    using Drain = std::function<bool()>;  // loop 内执行，返回是否还有结果；不得抛异常。
    using Idle = std::function<void()>;   // drain 已退出且无待调度通知；不得抛异常。
    using Post = std::function<void(std::function<void()>)>;

    // 默认使用 loop.queueInLoop。注入 Post 必须跨线程安全、只延后投递；
    // 抛异常必须意味着尚未接纳回调。仅测试故障注入需要替换。
    CompletionPump(muduox::net::EventLoop& loop, Drain drain, Idle idle, Post post = {});
    ~CompletionPump();
    CompletionPump(const CompletionPump&) = delete;
    CompletionPump& operator=(const CompletionPump&) = delete;

    void notify() noexcept;  // 任意线程；宿主必须活到最后一个通知者退出。
    bool idle() const;       // 仅所属 loop；不包括宿主的 worker 是否已退出。
    std::size_t postFailures() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace anof::runtime
