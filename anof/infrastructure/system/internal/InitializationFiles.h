#pragma once

#include <anof/application/ports/InstanceInitialization.h>
#include <sys/types.h>
#include <string_view>

namespace anof::infrastructure::system::detail {
InitResult<InitializationIdentity> parsePending(std::string_view, InitStage);
InitResult<InstanceId> parseFormat(std::string_view, InitStage);
std::string pendingText(const InitializationIdentity&);
std::string formatText(const InitializationIdentity&);
InitFailure initializationError(ErrorCode, InitStage);
InitFailure initializationErrno(int, InitStage);

// 局部故障接缝；生产工厂固定真实系统调用，不安装全局回调。
struct InitializationFileOps {
    ssize_t (*write)(int, const void*, std::size_t);
    int (*sync)(int);
    int (*renameNoReplace)(int, const char*, int, const char*);
    int (*unlink)(int, const char*, int);
};
InitializationFileOps initializationFileOps();
InitResult<std::unique_ptr<InstanceInitializer>> makeLinuxInstanceInitializerWith(InitializationFileOps);
}  // namespace anof::infrastructure::system::detail
