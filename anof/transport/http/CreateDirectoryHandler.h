#pragma once

#include <anof/application/ports/CreateDirectoryDispatcher.h>
#include <anof/transport/http/HttpApi.h>

namespace anof::http {

struct DirectoryTaskRecord {
    RequestId requestId;
    std::uint64_t taskId;
    std::optional<CreateDirectoryAttempt> attempt;
    bool cancelledBeforeStart;
    std::optional<ErrorCode> error;
    bool exchangeActive;
};
using DirectoryTaskObserver = std::function<void(DirectoryTaskRecord)>;

class CreateDirectoryHandler {
public:
    // 依赖必须覆盖所有已接纳任务的完成交付；observer 非阻塞且有界。
    CreateDirectoryHandler(CreateDirectoryDispatcher& dispatcher, std::function<bool()> ready,
                           DirectoryTaskObserver observer = {});
    // 返回的 callable 拥有配置，不借用 handler 对象；每个请求独立持有状态。
    CreateDirectoryStream stream() const;

private:
    CreateDirectoryDispatcher* dispatcher_;
    std::function<bool()> ready_;
    DirectoryTaskObserver observer_;
};

}  // namespace anof::http
