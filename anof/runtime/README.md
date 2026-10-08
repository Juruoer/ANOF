# Runtime 模块

本模块为创建目录用例提供有界异步调度、完成结果回投和生命周期协调。业务操作在独立的元数据 worker 执行，完成回调在所属 muduox EventLoop 执行。本文面向维护和扩展该模块的开发人员，说明现有接口、并发约束及测试入口。

## 1. 接口与依赖

| 文件 | 作用 |
|---|---|
| [CreateDirectoryDispatcher.h](../application/ports/CreateDirectoryDispatcher.h) | transport 依赖的提交／取消／完成端口，无 muduox、SQLite 或 JSON 类型 |
| [MetadataExecutor.h](MetadataExecutor.h) | 实现调度端口，管理一个 metadata worker、待执行及完成 ring |
| [CompletionPump.h](CompletionPump.h) | 合并跨线程通知、每轮有界消费、投递异常恢复 |
| [AnofRuntime.h](AnofRuntime.h) | worker-ready、HTTP-started、HTTP-stopped 与 worker-stopped 的生命周期协调 |

[CMakeLists.txt](CMakeLists.txt) 定义静态库 `anof_runtime`，依赖 `anof_application`、`muduox_net` 和 `Threads::Threads`，使用 C++17。数据库、时钟和节点 ID 生成器由工厂注入；模块不依赖具体数据库或 crypto 实现。

## 2. 调度与容量

`MetadataExecutor(loop, options)` 在 loop 线程构造。`maxPending` 默认 256，必须大于 0 且其加一可表示；`drainBatch` 默认 32，允许 1～32。两个 ring 在构造时分配，启动前分配失败会抛异常；不会接纳业务后再扩容完成存储。

`start(factory, started, stopped)` 只接受一次启动。空工厂、重复启动为同步 `InvalidArgument`，线程创建失败为同步 `InternalError`；这些拒绝不触发回调。启动被接受后，factory 在 worker 调用，返回独占的 `MetadataStore`、`Clock`、`NodeIdGenerator`。三个对象都在 worker 使用和销毁；禁止把 loop 中已打开的数据库放入工厂闭包再搬到 worker。空资源或工厂抛异常产生异步启动失败，仍完成停止清理。

成功打开资源后在 loop 延后调用 `started(Result<void>)`。其成功只代表 metadata 初始化完成，不能当作整个 HTTP 服务 READY；若已请求停止，成功的初始化通知也不会重新开放准入。用户回调可提交任务或请求停止，但必须短小、不阻塞，不能在回调内销毁 executor／runtime。loop 必须比这些对象活得更久。

`trySubmit` 的 Accepted 持有 `unique_ptr<TaskHandle>`；拒绝值为 `QueueFull`、`Stopping`、`InternalError`。未启动、正在启动或已停止均按不可接纳的 `Stopping` 返回；空完成回调、内部任务序号耗尽或接纳前分配失败为 `InternalError`。拒绝后没有第二次异步通知。

接纳同时检查待执行条数 ≤ Q 和未归还任务总数 ≤ Q+1。总数包含排队、运行、完成积压及正在 loop 调用其完成回调的任务。即使 HTTP 已取消，或 pending ring 已空，只要完成未消费就不能释放总名额；重入回调提交时也不能提前借用当前任务的名额。完成 ring 长度为 Q+1，完成发布只移动已有 Job 和结果，不重新分配业务存储。

每个 Accepted 恰有一次 loop 完成，内容是 `Result<CreateDirectoryResult>` 或 `CancelledBeforeStart`，并带 taskId 与可选的 `CreateDirectoryAttempt`。taskId 是 executor 内部递增关联号，与 request ID、候选节点 ID 不同，不是幂等键。提交参数及回调闭包须有界；执行器不解析或校验 HTTP 正文，接入层应先限制输入大小并构造自有的业务值。完成回调只弱引用 HTTP 请求状态。这里限制任务数量，不限制调用方任意闭包大小、总 RSS 或 socket 数。

句柄可移动、可以比 executor 活得更久，销毁不取消任务。`cancelBeforeStart()` 可跨线程调用，通过 CAS 与 worker 的 Queued → Running 竞争。取消成功只决定不再执行，并不立刻回收队列条目；重复取消返回 AlreadyTerminal，不代表完成回调已消费。worker 已 Running 时返回 AlreadyRunning，允许事务正常完成，不能因此假定回滚。

执行异常保守映射为 `CommitOutcomeUnknown`，保留已生成的 attempt，绝不自动重试。应用正常返回的错误保持原分类。任务／完成回调／启动和停止回调异常均有隔离，`snapshot()` 提供对应计数；AnofRuntime 自己的最终停止通知异常另由 `notificationExceptions()` 计数。业务完成回调及闭包在 loop 销毁，然后才释放任务名额。

## 3. 完成回投与异常恢复

CompletionPump 只使用 muduox 的公开 EventLoop／TimerId API，不依赖标为内部的 Channel。正常路径将通知合并为至多一个待执行 `queueInLoop` 回调；执行期间新通知只置 dirty 标志。drain 的结束检查与通知标志共用同步边界，避免空队列检查附近丢失唤醒。每轮最多消费配置的 batch，剩余结果延后到下一轮；并不为每个任务投递一个 functor。

`queueInLoop` 分配可能抛异常，不能因此丢弃已接纳任务。构造时先注册一个固定 10ms 恢复定时器；投递异常时保留 scheduled／recovery 标志和原结果，由该定时器在 loop 消费。启动失败、停止通知与 worker-exited 不占用业务队列槽位，使用固定控制状态和同一个合并通知。`postFailures()`／snapshot 记录投递异常。

恢复定时器也非阻塞观察 thread-exit future：worker 先销毁全部资源再通知完成，future 在真正线程退出时就绪，loop 不提前 join。若最后一次通知先于线程退出到达，后续 timer tick 再检查即可，不反复自投递空回调。登记 thread-exit 本身若分配失败，会在执行工厂前拒绝启动；这一特殊路径在最后一次通知后设置 future，就只剩不访问业务资源或 loop 的返回尾部。10ms 是调度周期，不是实时截止保证；loop 阻塞或底层框架本身失败时不承诺可恢复。这里只覆盖可捕获的投递失败，不声称能从任意系统性内存耗尽恢复。

Pump 的注入 Post 仅用于边界测试：必须线程安全、延后执行，且抛异常表示回调尚未接纳。不能改用可能内联执行的 runInLoop。内部 drain／idle 回调不得抛异常；外部业务回调异常由 executor 隔离。

## 4. 停止与生命周期协调

`MetadataExecutor::requestStop()` 幂等，立即关闭 loop 侧准入，并将仍在队列中的 Queued 项原子标为 Cancelled。Running 工作不被强关；worker 跳过取消任务、发布每个终态，在自身线程销毁资源并退出。loop 继续消费结果，待 thread-exit future 就绪、全部名额归还、pump 无待执行通知后才 join 并报告 Stopped。

析构不隐式阻塞 join，也不 detach。必须先停止并驱动 loop 到 Stopped，再在回调之外析构；违反生命周期约束会 fail-fast，防止活跃 worker 或悬空 loop 被静默遗留。正常析构取消恢复 timer。尚未启动的 executor 可以直接析构或停止。

AnofRuntime 持有 MetadataExecutor，并记录外部 HTTP 服务的启动／停止信号。它不创建或持有 HttpServer、实例锁、迁移器、配置或日志 sink；这些资源由调用方管理。

| 接口／事件 | 当前行为 |
|---|---|
| `start(factory, metadataStarted, stopped)` | 启动 metadata worker；初始化成功不直接将 runtime 设为 Ready |
| `markHttpStarted()` | 仅在 metadata 初始化成功且仍为 Starting 时接纳，使 `ready()` 为 true；调用方须已成功启动 HTTP |
| `requestStop()` | 关闭 metadata 准入，令 ready=false，进入 Stopping；未启动 HTTP 时视其停止屏障已满足 |
| `markHttpStopped()` | 仅在 Stopping 且已登记 HTTP 启动时接纳一次；必须由实际 HTTP 清理完成通知触发 |
| metadata Stopped 与 HTTP stopped 都已满足 | 进入 RuntimeState::Stopped，并调用最终停止通知 |

HTTP 和 metadata 的停止完成顺序可以互换。调用方在 `requestStop()` 后负责停止外部 HttpServer，并在其停止 callback 中调用 `markHttpStopped()`；不能提前发送这一信号，也不能只看到 worker Stopped 就释放整个实例。启动失败同样需要释放调用方拥有的资源。

最终停止通知允许请求 loop 退出。runtime 必须在自身回调栈之外销毁，且所属 loop 仍需存活。`markHttpStarted()`／`markHttpStopped()` 只是调用方报告的生命周期信号，本模块不独立验证 HTTP 服务的实际状态。

## 5. 测试入口

测试使用 Boost.Test，并在 `ANOF_BUILD_TESTS` 开启时注册到 CTest。

| 测试目标与源码 | 覆盖内容 |
|---|---|
| `anof_runtime_unittest`：[Runtime_unittest.cc](tests/Runtime_unittest.cc) | 队列与未归还总数耗尽、完成积压、32 条 drain 让出 loop、回调重入、一次完成、取消顺序与竞争、过期请求弱引用、任务与通知异常、投递 bad_alloc 恢复、启动失败、启动中停止、资源线程归属、线程局部析构阻塞及两侧停止屏障 |
| `anof_metadata_executor_integration_unittest`：[MetadataExecutor_unittest.cc](../../tests/integration/MetadataExecutor_unittest.cc) | 在 worker 内开关真实 SQLite 连接，验证创建成功、无效 Session、同名冲突及关闭后重开可见的目录、父 revision、计费和审计 |
| `anof_http_create_directory_integration_unittest`：[HttpCreateDirectory_unittest.cc](../../tests/integration/HttpCreateDirectory_unittest.cc) | 真实 HTTP、生产 crypto／Clock、runtime 与 SQLite 装配；Session 复验、私有隔离、配额、回滚、不确定提交、超时迟到结果、超时提交后重发冲突、真实提交结果占满完成名额，以及 HTTP／worker 两种停止顺序 |

单元测试通过确定性 gate 和有限超时控制线程先后关系，数据库集成使用小型临时 fixture。生命周期单元测试使用显式 HTTP 屏障信号，不覆盖真实 HTTP socket 或完整服务停止；HTTP 集成测试使用实际 HttpServer stop callback，装配仅存在于测试目标，不含生产入口、实例锁或配置加载。小样本数据库测试不验证硬件掉电持久性。

## 6. 实例装配

`anof_instance_runtime` 在基础调度库之上链接 HTTP 和基础设施适配器，提供 [InstanceServer](InstanceServer.h)。构造参数为持有实例锁的 `PreparedInstance`、loop 侧 crypto 和 metadata worker 工厂；[makeInstanceServer](InstanceServer.cc) 使用生产 libsodium、SystemClock 和 SQLite 工厂，数据库固定为实例根下的 `.anof/metadata.sqlite3`。它不创建数据库、账户或默认凭据。

[prepareInstance](../application/maintenance/PrepareInstance.h) 按取得排他 lease、读取配置、校验配置、执行维护检查的顺序准备实例。锁、配置与维护是独立类型化端口；具体 Linux 锁、TOML 和 SQLite 维护适配由 [infrastructure](../infrastructure/README.md) 提供，入口负责组合。准备操作在监听之前同步完成，不在请求回调内执行。配置只接纳显式启用的 `127.0.0.1` HTTP，队列上限 256、HTTP 在途上限 128、停止宽限 0～30 秒；端口 0 由内核分配。未启用本机开发 HTTP 的配置会被拒绝。

InstanceServer 构造不绑定端口。`start()` 接纳启动后在 worker 创建业务资源，初始化成功才构造、配置并启动 HttpServer，随后公开 ready 和实际端口。`port()` 在首次成功启动前为空，停止后保留原端口供诊断。同步拒绝不调用通知；接纳启动后 ready 通知一次，初始化中停止以 Retryable 表示取消。ready 通知抛异常会触发停止，停止通知抛异常计入 `notificationExceptions()`。

`requestStop()` 先关闭业务准入，再请求真实 HTTP 清理。实例停止必须同时满足 metadata 退出与 HTTP 清理屏障；HTTP 部分启动后抛出的可捕获异常也走清理路径。调用者保持 loop 运行至 `state()==Stopped`，在回调栈外销毁 InstanceServer，最后销毁 loop。实例 lease 在 HTTP、runtime、crypto 和回调闭包销毁之后释放；仅到 Stopped 不会提前释放锁。对象不能重启。

当前 muduox 的 HttpServer 构造即执行 bind，socket／bind／listen 失败可能直接 fatal 终止进程。这类失败不能转换成 ready 错误回调；操作系统会回收文件描述符，但这不等同于正常执行了停止屏障。装配不做存在竞争窗口的“预先探测端口”，也没有修改 muduox。

[InstanceServer 集成测试](../../tests/integration/InstanceServer_unittest.cc) 使用真实 SQLite、crypto、时钟和回环 HTTP，覆盖延迟绑定、缺失数据库、worker 初始化异常、启动中取消、重复停止及回调异常；基础案例的 lease 使用测试替身，另有真实 lease 与 worker gate 的组合案例检查初始化取消和析构释放；跨进程排他及信号关闭由 [serve 进程集成测试](../../tests/integration/ServeProcess_unittest.cc) 覆盖。命令行使用说明见 [前台服务入口](../../apps/anof/README.md)。
