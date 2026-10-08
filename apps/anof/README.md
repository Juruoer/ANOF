# 前台服务入口

`anof` 是 Linux 前台服务可执行文件，使用 CLI11 解析命令。它支持已有实例的本机开发 HTTP 服务：

```sh
cmake --build cmake-build-debug --target anof --parallel 2
./cmake-build-debug/apps/anof/anof serve --root /absolute/path/to/instance
./cmake-build-debug/apps/anof/anof serve --help
```

`--root` 必填，必须满足[实例路径与权限契约](../../anof/infrastructure/InstanceStartupContract.md)。程序不创建实例、配置、数据库、账户或默认凭据；没有 init／登录命令。现有数据库中没有有效 Session 时，业务请求无法通过认证。配置必须显式设置 `development_http=true`，仅监听 `127.0.0.1`；这不是远程部署或完整用户认证入口。

[Main.cc](Main.cc) 负责命令解析；[Serve.cc](Serve.cc) 创建真实锁、TOML 和 SQLite 维护适配，调用 prepareInstance，再把持锁结果移交 InstanceServer。数据库维护在监听前完成，业务连接由 metadata worker 独立打开。根依赖包括 CLI11；服务目标链接 process_support、instance_runtime 和 infrastructure，不链接测试播种器。

## 启动、停止及输出

程序在线程创建之前屏蔽 SIGINT／SIGTERM，完成实例准备、crypto 和 worker 初始化后才启动 HTTP。启动成功在 stdout 输出并刷新一行：

```text
ANOF_READY address=127.0.0.1 port=8080
```

配置 port=0 时，该行包含内核分配的实际端口。stdout 也可能包含 muduox 日志，不能将第一行输出直接当作就绪通知。就绪行只说明当前已完成启动，持续健康状态由 `/health/ready` 提供。

所属 loop 每 50ms 非阻塞读取停止信号。首次收到停止信号输出 `ANOF_STOPPING`，关闭业务准入并请求 HTTP 停止；重复信号不延长宽限、不强制终止运行中的数据库任务。HTTP 和 metadata 两侧屏障完成后退出 loop，在回调栈外销毁服务对象，最后释放 lease。服务和 loop 销毁后输出 `ANOF_STOPPED`；退出前恢复原信号屏蔽集。

同步维护期间不强行中断 SQLite；维护返回后检查已收到的信号，可在创建 worker 前直接退出。HTTP 宽限不是整个进程的强制退出期限；底层 worker I/O 未结束时继续持锁等待，由外部服务管理器制定独立终止策略。

可报告错误写入 stderr，形式为 `ANOF_ERROR stage=<固定阶段> code=<稳定分类>`。不输出配置内容、路径或异常 what()。命令行错误提示使用 `--help`。退出码如下：

| 退出码 | 含义 |
|---|---|
| 0 | 帮助输出或正常信号停止，包括监听前取消 |
| 2 | 命令或参数语法错误 |
| 3 | 实例准备失败，例如锁竞争、非法配置、缺失／损坏元数据 |
| 4 | 信号桥接初始化、适配构造、服务装配或启动失败，以及未分类入口异常 |
| 5 | 服务期间信号读取或通知异常 |

当前 muduox 在 socket／bind／listen 失败时可能直接终止进程，该行为不经过上述正常退出码和停止屏障。端口冲突可能表现为 SIGABRT，操作系统回收锁不等于执行了干净停止。程序使用现有公开接口，不预先探测端口或修改子模块。日志输出沿用 muduox 的默认行为，尚无独立的有界访问日志 worker。

## 停止信号组件

`anof_process_support` 提供 [ShutdownSignals](ShutdownSignals.h)，不安装异步信号处理器。`create()` 保存原屏蔽集并创建非阻塞、close-on-exec 的 signalfd；新 worker 继承信号屏蔽集。进程只能存在一个该对象，重复创建返回 Conflict。

`poll()` 一次最多读取两条记录，返回 true 后保持为 true；读取失败返回 InternalError，跨线程调用返回 InvalidArgument。它不直接调用 runtime。必须在所有 worker 退出后由创建线程销毁，关闭 fd 并恢复原屏蔽集；恢复后到达的信号遵循原进程策略。它不能与其他 SIGINT／SIGTERM 消费者并用，也不能补救此前启动且未屏蔽信号的线程。

## 测试入口

[信号测试](tests/ShutdownSignals_unittest.cc) 检查信号读取、重复注册、屏蔽集恢复、线程继承和跨线程误用。[进程集成测试](../../tests/integration/ServeProcess_unittest.cc) 启动真实可执行文件，覆盖帮助／错误参数、实际 HTTP、无默认认证、第二进程拒绝、准备失败、SIGINT／SIGTERM 停止、活动连接清理及重启；独立子进程禁用 core dump 后验证现有端口冲突行为。[实例服务测试](../../tests/integration/InstanceServer_unittest.cc) 还通过真实 lease 和 worker gate 检查初始化取消与析构释放顺序。

```sh
cmake --build cmake-build-debug --target anof_serve_process_integration_unittest anof_instance_server_integration_unittest --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R 'anof_(serve_process|instance_server)_integration_unittest' -j 1
```

测试只使用私有临时实例和小型数据库，不提供生产初始化捷径，也不证明物理机掉电持久性或组合背压行为。
