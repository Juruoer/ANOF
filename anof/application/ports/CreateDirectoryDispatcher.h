#pragma once

#include <anof/application/drive/CreateDirectory.h>
#include <anof/application/ports/TaskDispatcher.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

namespace anof {

struct CreateDirectoryCompletion {
    std::uint64_t taskId;
    std::variant<Result<CreateDirectoryResult>, CancelledBeforeStart> outcome;
    std::optional<CreateDirectoryAttempt> attempt;
};

using CreateDirectoryCallback = std::function<void(CreateDirectoryCompletion)>;

class CreateDirectoryDispatcher {
public:
    virtual ~CreateDirectoryDispatcher() = default;
    // 仅所属 loop 调用。Accepted 保证一次延后的 loop 完成回调（包括取消）；拒绝不再回调。
    // context/command 必须自有；callback 只弱引用 HTTP 状态，不捕获 borrowed 视图或 writer。
    // 命令及闭包须有界；当前 HTTP 调用方先执行 64 KiB 正文与 DTO 校验。
    virtual TaskSubmission trySubmit(RequestContext context, CreateDirectoryCommand command,
                                                CreateDirectoryCallback completion) = 0;
};

}  // namespace anof
