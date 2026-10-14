#pragma once

#include <anof/core/Result.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

namespace anof {
enum class CancelBeforeStartResult { Cancelled, AlreadyRunning, AlreadyTerminal };
class TaskHandle {
public:
    virtual ~TaskHandle() = default;
    // 与阶段执行原子竞争；不终止运行事务。销毁句柄不隐式取消，不代表完成名额已归还。
    virtual CancelBeforeStartResult cancelBeforeStart() noexcept = 0;
    virtual std::uint64_t id() const noexcept = 0;
};
struct CancelledBeforeStart {};
struct AcceptedTask { std::unique_ptr<TaskHandle> handle; };
enum class SubmitError { QueueFull, Stopping, InternalError, RateLimited };
using TaskSubmission = std::variant<AcceptedTask, SubmitError>;

template <typename T>
struct TaskCompletion {
    std::uint64_t taskId;
    std::variant<Result<T>, CancelledBeforeStart> outcome;
};
template <typename T>
using Completion = std::function<void(TaskCompletion<T>)>;
}  // namespace anof
