#pragma once
namespace anof::app {
// 仅检查运行库，不打开实例、不初始化网络或启动线程；成功返回nullptr。
const char* runtimeDependencyFailure() noexcept;
}
