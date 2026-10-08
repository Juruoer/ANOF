#pragma once

#include <anof/application/drive/CreateDirectory.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

namespace anof {

enum class CancelBeforeStartResult { Cancelled, AlreadyRunning, AlreadyTerminal };

class TaskHandle {
public:
    virtual ~TaskHandle() = default;
    // 可跨线程取消；与 Queued -> Running 原子竞争，不中断正在运行的事务。
    // 销毁句柄不隐式取消；句柄可以比 dispatcher 活得更久。
    // AlreadyTerminal 也包括已决定取消但尚未回投；不是名额已归还的证明。
    virtual CancelBeforeStartResult cancelBeforeStart() noexcept = 0;
    virtual std::uint64_t id() const noexcept = 0;
};

struct CancelledBeforeStart {};

struct CreateDirectoryCompletion {
    std::uint64_t taskId;
    std::variant<Result<CreateDirectoryResult>, CancelledBeforeStart> outcome;
    std::optional<CreateDirectoryAttempt> attempt;
};

struct AcceptedTask {
    std::unique_ptr<TaskHandle> handle;
};

enum class SubmitError { QueueFull, Stopping, InternalError };
using CreateDirectorySubmission = std::variant<AcceptedTask, SubmitError>;
using CreateDirectoryCallback = std::function<void(CreateDirectoryCompletion)>;

class CreateDirectoryDispatcher {
public:
    virtual ~CreateDirectoryDispatcher() = default;
    // 仅所属 loop 调用。Accepted 保证一次延后的 loop 完成回调（包括取消）；拒绝不再回调。
    // context/command 必须自有；callback 只弱引用 HTTP 状态，不捕获 borrowed 视图或 writer。
    // 命令及闭包须有界；当前 HTTP 调用方先执行 64 KiB 正文与 DTO 校验。
    virtual CreateDirectorySubmission trySubmit(RequestContext context, CreateDirectoryCommand command,
                                                CreateDirectoryCallback completion) = 0;
};

}  // namespace anof
