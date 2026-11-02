# 测试导航与注册约定

想检查一个组件，可以从它自己的测试目录开始；想检查多个模块组合起来是否正常，则看这里的集成、共享后端和部署测试。部分名称含 unittest 的测试也使用真实文件或子进程，选择测试时要看它实际覆盖的流程。

## 执行

依赖准备和 Presets 见 [构建说明](../BUILDING.md)。从仓库根目录执行：

```sh
./scripts/build.sh --preset debug-tests --test
./scripts/build.sh --preset release-tests --test
```

限定范围时先构建对应 target，再用 `ctest --test-dir cmake-build-debug --output-on-failure -j 1 -R '<测试名称正则>'`。`ctest --test-dir cmake-build-debug --show-only=json-v1` 可检查注册名称、超时和标签。关闭测试的 preset 不查找 Boost/Python。

| 目录 | 验证对象 |
| --- | --- |
| [persistence_contract](persistence_contract/CMakeLists.txt) | 实际后端必须满足的事务和仓储语义 |
| [integration](integration/CMakeLists.txt) | 应用、调度、HTTP、CLI 与恢复的组合行为 |
| [support](support/README.md) | 事件循环有限等待及诊断辅助 |
| [deployment](deployment/README.md) | 显式配置的 nginx/TLS 与安装夹具 |
| [build](build/CMakeLists.txt) | 启动时拒绝不合要求的运行依赖 |
| [diagnostics](diagnostics/README.md) | 独立时钟诊断工具 |

Web 单元和浏览器测试由 [Web 入口](../web/README.md) 管理，不由 CTest 隐式下载或运行。可选 TLS/安装测试有独立前提；未注册不能当作通过。

## 相似测试分别检查什么

同一业务可能在几个目录中出现。选择要运行的测试时，可以沿着改动经过的层次查找：

| 改动涉及的行为 | 需要一起考虑的测试 |
| --- | --- |
| 值校验、额度计算、树规则 | core/domain 的纯规则测试，以及调用这些规则的应用集成测试 |
| 仓储事务与失败回滚 | infrastructure 的具体适配器故障测试，以及 persistence_contract 的共享语义测试 |
| 应用授权和多项写入 | integration 中的 Service/CreateDirectory 测试，检查真实事务、审计及失败后的持久状态 |
| 异步调度与停止 | runtime 的队列、回调和线程测试，以及 integration 中 Executor/InstanceServer 的真实资源寿命测试 |
| HTTP 请求和浏览器身份 | transport 的解析、映射和受控端口测试，以及 integration 中 Http 测试的真实身份和数据库行为 |
| 客户端、命令与终端 | client 的协议和本地状态测试、apps 的参数/PTY 测试，以及 integration 中 ClientProcess/ServeProcess 的可执行程序流程 |
| 检查、修复、备份与恢复 | 维护共享契约、Fsck、MaintenanceStartup 和 OfflineMaintenance 分别检查端口、修复证据、启动拒绝和命令执行 |

例如，上传 HTTP 测试通过网络检查偏移和响应，UploadExecutor 测试直接控制磁盘任务与恢复阶段，传输 CLI 测试还要检查跨进程保存的本地记录。即使都上传了同样的小文件，它们也不能互相替代。部署测试另行检查 TLS 代理和安装目录，浏览器测试则检查 Cookie、页面、Worker 和 IndexedDB。

共享头文件检查也有不同的链接范围：只依赖应用端口的目标可以发现后端类型泄漏，完整装配的目标检查公开入口是否能独立包含。不要仅因为包含了同一个头文件就合并它们。

## 添加或整理测试

Boost.Test 注册复用 [AnofTests.cmake](../cmake/AnofTests.cmake)。例如本地测试目录可采用：

```cmake
anof_add_unit_test(anof_example_unittest Example_unittest.cc)
set_tests_properties(anof_example_unittest PROPERTIES TIMEOUT 30 LABELS "unit")
target_sources(anof_example_unittest PRIVATE Fixture.cc)
target_link_libraries(anof_example_unittest PRIVATE anof_application)
target_compile_options(anof_example_unittest PRIVATE -Wall -Wextra -Wconversion -Wshadow)
```

这里的 Example 和 Fixture 是示例文件名，需要换成自己的源码。helper 默认链接领域库和 Boost、设置动态链接宏并注册测试；真实跨模块用例应覆盖标签为 integration 并给出有限超时。共享持久化/传输契约使用既有 factory helper。Python 或外部命令直接使用 `add_test`，显式设置执行器、依赖、超时和标签。独立头文件编译是构建检查，不注册虚假的运行测试。

保持显式源列表与 target 作用域，不增加本地重复 helper。整理前后比较注册清单；相同断言在不同层可能保护不同边界。删除用例需要对应的保留覆盖，不能用取消测试、自动重试或扩大期限处理偶发失败。

普通测试使用独立小型临时目录、串行运行；单文件不超过 2 GiB，所有生成数据合计低于 50 GiB，常规夹具尽量低于 1 GiB。小文件、故障注入和进程退出不证明硬件掉电耐久性或大规模性能。
