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

## 业务 UTC 与上传失败诊断

设置 `ANOF_TRANSFER_DIAGNOSTICS=1` 后，上传应用在进度有效期计算、仓储替换或事务提交失败时，
向 stderr 输出 `ANOF_TRANSFER_DIAGNOSTIC`。默认关闭，不改变时钟返回值、事务结果、重试或期限。
字段包括规范 upload_id、stage、错误分类及枚举数值、创建／前后进度／终态 UTC 微秒、generation和offset；
terminal_us=-1表示无终态时间。不输出凭据、名称、请求正文或SQL文本；诊断仍会影响执行时序。
`progress.expiry`与`save.replace`失败不能等同于SQLite COMMIT失败，真正提交阶段标为`save.commit`。
数值错误码仅用于当前源码定位，不是稳定wire协议。

根HTTP测试失败会显示method、无query的path、期望状态和响应；下载夹具的PATCH另显示offset/length。
上传子进程夹具失败打印错误枚举及UTC前后值、steady经过时间，父进程区分提前退出与计划SIGKILL。
CLI进程失败只输出命令类别、stdout/stderr长度和白名单错误字段，不回显参数值、PTY、原始message或凭据。
服务子进程退出时最多转录16条固定格式上传诊断，避免其stderr随夹具销毁丢失。

一次有界诊断可使用：

```sh
ANOF_TRANSFER_DIAGNOSTICS=1 BOOST_TEST_LOG_LEVEL=test_suite \
ctest --test-dir cmake-build-debug -V --output-on-failure -j 1 \
  -R '^(anof_account_service_integration_unittest|anof_upload_executor_integration_unittest|anof_http_upload_integration_unittest|anof_http_download_integration_unittest|anof_client_protocol_test|anof_client_process_integration_unittest|anof_transfer_client_process_unittest)$'
```

[账户及上传应用测试](../integration/AccountService_unittest.cc) 的 `clock_rollback_*` 用例注入可控业务时钟，
核对上传回拨下进度、终态及保留量，以及账户更新时间与提交响应的一致性；
[客户端协议测试](../../anof/client/tests/Protocol_unittest.cc) 核对逆序时间响应被分类为未知写结果。
[HTTP 身份集成测试](../integration/HttpIdentity_unittest.cc) 还覆盖回拨与服务重启后的真实响应及客户端解码。
客户端拒绝畸形逆序响应的用例继续保留，不放宽协议校验。
真实系统时钟的UploadExecutor用例没有注入回拨。日志中的CommitOutcomeUnknown也可能来自原有故障注入，
必须结合用例边界和stage判断，不能仅凭某一条诊断认定环境故障。以上命令不修改宿主／WSL时间。
