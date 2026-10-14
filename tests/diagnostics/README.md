# 时钟与等待诊断

此目录提供按需使用的诊断工具，不修改系统时间，也不改变生产定时器或测试超时值。

- `observe_clocks.py`：使用 Python 标准库，每 20ms 采样系统时间与单调时钟，串行运行四组 HTTP 测试、
  ServeProcess 测试和独立定时器探针。每轮六个进程；默认三轮，最多十轮。单进程上限为 45 秒，探针为
  15 秒，超时会终止该次进程组。输出目录必须尚不存在，避免覆盖已有证据。
- `WaitTrace.h`：设置 `ANOF_WAIT_TRACE` 时输出等待调用位置、回调顺序、单调经过时间、系统经过时间和
  最近一次完成条件检查结果。不会为诊断额外执行有副作用的 predicate；`observed_done=0` 仅表示最近
  的检查未完成，不代表超时回调执行瞬间重新检查过。调用点由 GCC/Clang 的 C++17 内建函数捕获。
- `TimerProbe.cc`：只依赖 muduox EventLoop，运行 1ms 周期任务和三秒截止任务，报告实际经过时间、
  时钟偏移变化和最长周期回调间隔。该目标默认不构建、不注册为 CTest；返回零仅表示探针运行完成。

从项目根目录执行：

```sh
cmake --build cmake-build-debug --parallel 2 --target anof_timer_probe \
  anof_http_identity_handlers_unittest anof_http_identity_stream_unittest \
  anof_http_account_writes_unittest anof_http_create_directory_unittest \
  anof_serve_process_integration_unittest
python3 tests/diagnostics/observe_clocks.py --build cmake-build-debug \
  --output cmake-build-debug/clock-observation --rounds 3
```

`timeline.jsonl` 保存原始纳秒采样和进程开始/结束标记，各进程日志保留 Boost 用例边界和诊断输出。
C++ 输出的 `mono` 与 Python 的 `mono_ns / 1e9` 可在本项目 Linux 平台上对齐。以单调时间排序，
比较 `offset_change_ns` 和失败时间；探针的 `elapsed` 应与三秒目标比较，不能只看退出码。

此工具用于短时环境排查，不是压力测试或性能基准。采样线程也可能被调度延迟，因此跳变时刻只定位到
相邻采样之间；一次通过不能证明环境稳定。探针应通过 runner 运行，以保留独立的进程超时保护。

使用 `--serve-only` 可只运行 ServeProcess；runner 同时设置 `ANOF_SHUTDOWN_TRACE`，启用 runtime/serve
的阶段记录。直接运行程序时，默认不输出这些记录。`SHUTDOWN_TRACE` 包含 pid、单调/系统时间和事件：

- `serve`：消费停止信号、停止回调请求退出 loop、loop 返回、开始销毁实例。
- `identity` / `metadata`：请求停止、worker 最后通知/准备返回、退出 future 未就绪/就绪、join 后停止。
  `worker_returning` 仅表示即将返回，真正退出以 `exit_future_ready` 和 `stopped` 为准。
- `http`：开始 stop 和完整 stop 回调完成，不将响应结束当作连接已清理。
- `instance/barriers`：`a` 是 runtime 停止、`b` 是 identity 停止、`c` 是 HTTP 停止；runtime 本身也依赖
  HTTP，因此 metadata 是否已退出应结合 metadata 的 `stopped` 事件判断。
- `pump`：对象/状态对象的绑定记录，以及超过 100ms 的恢复轮询间隔（`a` 为毫秒数）。相邻的
  `bind_state` / `state` 启动记录对应同一个 pump；executor 的 object 是该 pump 地址。

ServeProcess 会记录父进程四秒退出等待的起止，并在诊断模式输出捕获的子进程日志。失败时额外读取
`/proc/<pid>` 的 timerfd、仍持有的 IPv4 TCP socket 和线程等待位置；此快照在原期限失败后获取，不
延长测试期限。TCP 行保留 Linux `/proc/net/tcp` 格式（第四列是状态，`05` 为 FIN_WAIT2）。
快照可能与进程并发变化，缺失行不是资源已清理的充分证据；不读取数据库、配置或凭据。

```sh
python3 tests/diagnostics/observe_clocks.py --serve-only --build cmake-build-debug \
  --output cmake-build-debug/shutdown-observation --rounds 3
```

诊断输出会影响线程调度，尤其最后通知与真正退出之间的窗口；观察到的先后关系只说明该次运行。
关停策略、线程 join 条件、grace 和父进程超时均未因诊断改变。
