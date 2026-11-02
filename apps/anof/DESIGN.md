# 命令装配与进程生命周期

`anof` 的各类命令共用参数解析和终端工具，但执行方式不同：serve 驱动事件循环，远程命令同步调用 HTTP
客户端，本地维护则等待离线工作线程完成。用户用法见 [命令参考](COMMANDS.md)。

## 参数解析与装配

[Main.cc](Main.cc) 负责命令解析；[Serve.cc](Serve.cc) 创建真实锁、TOML 和 SQLite 维护适配，调用 prepareInstance，再把持锁结果移交
InstanceServer。业务连接由元数据线程打开，密码和磁盘资源在各自的工作线程初始化。资源就绪后，runtime 先恢复上传、待删除对象和空间计数，再开始监听
HTTP；具体顺序见 [runtime 生命周期](../../anof/runtime/DESIGN.md)。

CLI11 负责参数解析。服务命令通过 process_support、instance_runtime 和 infrastructure 装配依赖；离线初始化通过
`anof_init_support` 和 `anof_instance_setup`
链接同一套应用与基础设施实现。目标之间的链接关系见 [CMakeLists.txt](CMakeLists.txt)。

客户端库接口、HTTP/文件安全边界见 [client 维护说明](../../anof/client/README.md)。[ClientCommands](ClientCommands.h)
注册命令、验证参数并调度类型化 API；[ClientOutput](ClientOutput.h) 只从受检 DTO 构造输出。`anof_client_commands` 链接
client、terminal_support、CLI11 和 JSON，不依赖服务端实现。最终可执行文件仍因 init/serve 包含服务端能力。

程序入口还在解析命令前检查 SQLite 和 libcurl 的实际运行版本与能力。失败输出 `stage=dependencies` 并退出
4，不打开实例。检查实现见 [RuntimeDependencies.cc](RuntimeDependencies.cc)。

## 终端与取消

客户端在同步命令期间安装可恢复的信号处理器，SIGINT/SIGTERM/SIGHUP/SIGQUIT/SIGTSTP 设置 lock-free 取消标志，由 libcurl
进度回调观察；不会启动服务端停止线程。终端读取仍使用屏蔽集和 signalfd，退出时恢复终端并保留取消状态。SIGPIPE
在客户端作用域被忽略，输出失败返回 5 并保留已确认的远端及本地状态；部分输出已经写出时无法撤回，调用者须同时检查退出码。退出后恢复原信号策略。

[InitOptions.cc](InitOptions.cc) 验证参数，[Init.cc](Init.cc) 调用初始化，[TerminalPassword.cc](TerminalPassword.cc)
管理终端和秘密输入，随后调用同步离线初始化装配。密码策略与服务端验证共用接纳上限，具体参数和密码任务调度见 [runtime 实现说明](../../anof/runtime/DESIGN.md)
。远程 login 命令通过 HTTP 认证，使用服务端的密码验证与登录限流。

## 离线报告与计划编码

JSON FSCK 输出为 header/finding/summary；文本模式输出摘要与报告位置，未指定报告时在外部工作目录生成随机报告名。报告和计划文件为
0600，独占创建，不能覆盖既有文件或写进源实例。详细 finding 最多 10000 条、每行 4096 字节，报告最多 48 MiB；计划最多 10000
动作、16 MiB、深度 16。外部计划拒绝重复/未知字段、非法 UTF-8、非规范 ID/数量，不接受 SQL
或任意内容路径。计划编码见 [MaintenancePlanJson](MaintenancePlanJson.h)。

## 停止信号组件

`anof_process_support` 提供 [ShutdownSignals](ShutdownSignals.h)，不安装异步信号处理器。`create()`
保存原屏蔽集并创建非阻塞、close-on-exec 的 signalfd；新 worker 继承信号屏蔽集。进程只能存在一个该对象，重复创建返回
Conflict。

`poll()` 一次最多读取两条记录，返回 true 后保持为 true；读取失败返回 InternalError，跨线程调用返回 InvalidArgument。它不直接调用
runtime。必须在所有 worker 退出后由创建线程销毁，关闭 fd 并恢复原屏蔽集；恢复后到达的信号遵循原进程策略。它不能与其他
SIGINT/SIGTERM 消费者并用，也不能补救此前启动且未屏蔽信号的线程。

## 测试入口

[信号测试](tests/ShutdownSignals_unittest.cc)
检查信号读取、重复注册、屏蔽集恢复、线程继承和跨线程误用。[进程集成测试](../../tests/integration/ServeProcess_unittest.cc)
启动真实可执行文件，覆盖帮助/错误参数、实际 HTTP、无默认认证、第二进程拒绝、准备失败、SIGINT/SIGTERM 停止、活动连接清理及重启；独立子进程禁用
core dump 后验证现有端口冲突行为。[实例服务测试](../../tests/integration/InstanceServer_unittest.cc) 还通过真实 lease 和
worker gate 检查初始化取消与析构释放顺序。

```sh
cmake --build cmake-build-debug --target anof_serve_process_integration_unittest anof_instance_server_integration_unittest --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R 'anof_(serve_process|instance_server)_integration_unittest' -j 1
```

测试使用私有临时实例，进程控制与底层组件检查分别保留，以便定位失败发生在哪一层。

[初始化参数测试](tests/Init_unittest.cc)、[终端密码测试](tests/TerminalPassword_unittest.cc) 分别检查受检数量及真实 PTY
的无回显、确认、超限与中断恢复。[初始化集成测试](../../tests/integration/InitializeInstance_unittest.cc)
检查真实密码、初始额度、准备门禁及发布失败后的
finish。初始化用例的逐阶段失败/异常覆盖见 [InitializeInstance_unittest.cc](../../anof/application/maintenance/tests/InitializeInstance_unittest.cc)。

[客户端进程验收](../../tests/integration/ClientProcess_unittest.cc) 通过真实可执行文件完成
init/serve、管理员邀请、两个独立用户注册登录、分页、私有空间隔离、额度拒绝、会话撤销和重启持久化。另以回环受控 peer 验证中断分类、真实
rename 失败、登录保存后的 EPIPE；PTY 验证秘密无回显、锁在提示前生效、超限/确认失败及终端恢复。

```sh
cmake --build cmake-build-debug --target anof_client_process_integration_unittest --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_client_process_integration_unittest$' -j 1
```

部署组合由[部署测试](../../tests/deployment/README.md)检查，浏览器行为由 Web 测试检查。
