# Runtime 模块

本模块为创建目录及身份／读取用例提供有界异步调度、完成结果回投和生命周期协调。业务操作在独立的元数据 worker 执行，完成回调在所属 muduox EventLoop 执行。本文面向维护和扩展该模块的开发人员，说明现有接口、并发约束及测试入口。

## 1. 接口与依赖

| 文件 | 作用 |
|---|---|
| [TaskDispatcher.h](../application/ports/TaskDispatcher.h) | 公共任务句柄、TaskSubmission 接纳结果与类型化完成值 |
| [CreateDirectoryDispatcher.h](../application/ports/CreateDirectoryDispatcher.h) | transport 依赖的提交／取消／完成端口，无 muduox、SQLite 或 JSON 类型 |
| [MetadataExecutor.h](MetadataExecutor.h) | 实现调度端口，管理一个 metadata worker、待执行及完成 ring |
| [IdentityExecutor.h](IdentityExecutor.h) | 身份／账户端口、多阶段任务及独立密码 worker |
| [AuthenticationLimiter.h](AuthenticationLimiter.h) | 来源／登录名有界令牌桶 |
| [CompletionPump.h](CompletionPump.h) | 合并跨线程通知、每轮有界消费、投递异常恢复 |
| [AnofRuntime.h](AnofRuntime.h) | worker-ready、HTTP-started、HTTP-stopped 与 worker-stopped 的生命周期协调 |

[CMakeLists.txt](CMakeLists.txt) 定义静态库 `anof_runtime`，依赖 `anof_application`、`muduox_net` 和 `Threads::Threads`，使用 C++17。数据库、时钟和节点 ID 生成器由工厂注入；基础库不依赖具体数据库或 crypto 实现。`anof_identity_resources` 中的 [InstanceIdentity](InstanceIdentity.h) 承担具体资源工厂。

## 2. 调度与容量

`MetadataExecutor(loop, options)` 在 loop 线程构造。`maxPending` 默认 256，必须大于 0 且其加一可表示；`drainBatch` 默认 32，允许 1～32。两个 ring 在构造时分配，启动前分配失败会抛异常；不会接纳业务后再扩容完成存储。

`start(factory, started, stopped)` 只接受一次启动。空工厂、重复启动为同步 `InvalidArgument`，线程创建失败为同步 `InternalError`；这些拒绝不触发回调。启动被接受后，factory 在 worker 调用，返回独占的 `MetadataStore`、`Clock`、`NodeIdGenerator`。可选 identity 资源包含身份 ID、凭证签发和邀请码 codec；身份任务要求这些资源非空。所有对象都在 worker 使用和销毁；禁止把 loop 中已打开的数据库放入工厂闭包再搬到 worker。空资源或工厂抛异常产生异步启动失败，仍完成停止清理。

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

`anof_instance_runtime` 在基础调度库之上链接 HTTP 和基础设施适配器，提供 [InstanceServer](InstanceServer.h)。构造参数为持有实例锁的 `PreparedInstance`、loop 侧 crypto、metadata worker 工厂和密码 worker 工厂；[makeInstanceServer](InstanceServer.cc) 使用生产 libsodium、SystemClock 和 SQLite 工厂，数据库固定为实例根下的 `.anof/metadata.sqlite3`。它不创建数据库、账户或默认凭据。

[prepareInstance](../application/maintenance/PrepareInstance.h) 按取得排他 lease、读取配置、校验配置、执行维护检查的顺序准备实例。锁、配置与维护是独立类型化端口；具体 Linux 锁、TOML 和 SQLite 维护适配由 [infrastructure](../infrastructure/README.md) 提供，入口负责组合。准备操作在监听之前同步完成，不在请求回调内执行。配置只接纳显式启用的 `127.0.0.1` HTTP，队列上限 256、HTTP 在途上限 128、停止宽限 0～30 秒；端口 0 由内核分配。未启用本机开发 HTTP 的配置会被拒绝。

InstanceServer 构造不绑定端口。`start()` 接纳启动后在 worker 创建业务资源，metadata 和密码资源初始化均成功才构造、配置并启动 HttpServer，随后公开 ready 和实际端口。`port()` 在首次成功启动前为空，停止后保留原端口供诊断。同步拒绝不调用通知；接纳启动后 ready 通知一次，初始化中停止以 Retryable 表示取消。ready 通知抛异常会触发停止，停止通知抛异常计入 `notificationExceptions()`。

`requestStop()` 先关闭业务准入，再请求真实 HTTP 清理。身份请求终态回收及密码线程退出后才停止 metadata；实例停止必须同时满足密码／metadata 退出与 HTTP 清理屏障；HTTP 部分启动后抛出的可捕获异常也走清理路径。调用者保持 loop 运行至 `state()==Stopped`，在回调栈外销毁 InstanceServer，最后销毁 loop。实例 lease 在 HTTP、runtime、crypto 和回调闭包销毁之后释放；仅到 Stopped 不会提前释放锁。对象不能重启。

HttpServer 使用 `DeferredListen` 构造和主 loop 同步 `tryStart()`，成功后才公布 ready。监听失败通过 ready 的稳定错误分类返回：bind 地址占用为 `Conflict`，权限不足为 `Forbidden`，文件描述符／内存／缓冲等临时资源不足或系统调用中断为 `Retryable`，其余错误及无效启动状态为 `InternalError`。不传播底层错误文本，不自动重试或预探测端口。监听系统错误返回前 muduox 已释放监听资源；InstanceServer 继续驱动 loop，等待密码和 metadata worker 退出及完成项回收，最后通知 Stopped。实例析构才释放 lease。内存分配、线程创建和库内部不变量故障不属于可恢复监听错误的保证范围。

[InstanceServer 集成测试](../../tests/integration/InstanceServer_unittest.cc) 使用真实 SQLite、crypto、时钟和回环 HTTP，覆盖延迟绑定、缺失数据库、worker 初始化异常、启动中取消、重复停止及回调异常；基础案例的 lease 使用测试替身，另有真实 lease 与 worker gate 的组合案例检查初始化取消和析构释放；跨进程排他及信号关闭由 [serve 进程集成测试](../../tests/integration/ServeProcess_unittest.cc) 覆盖。命令行使用说明见 [前台服务入口](../../apps/anof/README.md)。

监听失败集成验收覆盖真实端口冲突、两条 worker 的线程退出、ready／stopped 各通知一次、Stopped 后仍持有真实实例锁，以及析构后可重新获取锁。进程验收覆盖退出码 4、无 READY、有 STOPPED、释放端口后原实例可重新启动，且端口冲突不影响另一实例继续提供 HTTP 服务。

## 7. 离线初始化与密码策略

`anof_instance_setup` 提供 [initializeInstance / finishInitialization](InstanceInitialization.h)，只链接 application 与 infrastructure，不创建 EventLoop 或 HTTP。它构造真实文件初始化、数据库 Bootstrap、时钟、身份 ID 与密码适配器，调用 [初始化用例](../application/maintenance/InitializeInstance.h)。初始化 request ID 从独立请求身份适配器生成，不充当实体 ID。

[InstancePasswordPolicy.h](InstancePasswordPolicy.h) 是装配的唯一密码参数入口：新建 Argon2id v19 使用 65536 KiB、2 次、parallelism=1，verification 的三个上限分别为 65536/2/1。Store、Bootstrap、Maintenance 的 passwordHashLimits 与此一致。接纳上限允许范围内的小参数编码，但新初始化始终使用 hashing 参数；测试小参数不能改变生产装配策略。没有自动硬件校准；在线预算由 IdentityExecutor 另行限制。

普通初始化先校验参数、计算密码及生成 ID，再创建 .anof 并持同一 lease 执行配置写入、事务化引导、格式发布和 pending 清理。任何失败停止后续阶段；lease 析构只释放锁，不删除现场。finish 不构造密码适配器、不产生新身份，先验证当前 pending 的完整引导才发布。

[真实初始化测试](../../tests/integration/InitializeInstance_unittest.cc) 覆盖统一预算、密码验证、三组根计费、无默认 Session、重复 init、pending 拒绝启动及 finish 后可准备实例；不证明硬件掉电持久性。


## 8. 身份多阶段调度

[IdentityExecutor](IdentityExecutor.h) 实现 IdentityDispatcher 的十种身份／读取操作与七种账户操作。它借用已经 Ready 的 MetadataExecutor；所有数据库阶段使用其 `submitOperation`，与目录共用一个连接所属 worker、pending 和 completion 预算。transport 仅依赖 typed dispatcher，不能提交任意闭包。

一个密码 worker 使用与初始化相同的策略，启动时创建 hasher 和新盐 dummy hash；不存在账户也执行验证。最多8个密码请求包含读快照、等待计算、计算、最终事务及未消费完成；所有身份任务最多128个。这两个值可通过构造选项收紧。密码工作区至多一个64 MiB，另外保存有界输入、快照、dummy和结果；这不是进程 RSS 上限。

登录先读取并关闭快照，再验证密码，最后在新写事务复核 enabled、passwordVersion、authEpoch 和 revision 后签发七天绝对有效期 Session；每用户最多32个活跃 Session。注册先计算密码，再原子消费邀请、创建用户／私有根和额度、记录审计，不自动登录。登录／注册最终阶段之前至多每分钟执行一次独立清理事务，最多删除200个失效 Session；没有空闲周期清理。清理失败不会绕过限制。

AuthenticationSource 只接受可信 socket 地址，IPv4 统一为 mapped IPv6。令牌桶共享于登录、注册和改密：全局突发16、30次/分钟，来源突发5、10次/分钟，来源加规范登录名突发3、5次/分钟。最多1024条来源及组合记录，闲置10分钟回收，满表拒绝；时钟使用 steady_clock。登录／注册的限流在接纳前返回 SubmitError::RateLimited；改密先在metadata认证并取得规范登录名，再在所属loop检查同一限流器，以 Result 的 ErrorCode::RateLimited 延后完成。两者均表示速率拒绝（429），不同于 QueueFull（503）。

接纳前预留外层固定槽，跨阶段只有一个任务身份；完成通过 CompletionPump 合并通知，每轮最多32项。回调与闭包释放后才归还名额。取消可以与密码验证并行，避免进入最终事务；开始最终 metadata 阶段后的取消返回 AlreadyRunning，不能推断回滚。密码仍会运行到返回，停止屏障等待真实线程退出和全部完成。回调异常被隔离，不会再次完成。

InstanceServer 拥有该执行器并通过 `identityDispatcher()` 注入 HttpApi；构造参数位于目录 stream 之后、可选 observation/options 之前。生产实例暴露十九个身份／读取／账户HTTP操作及 mkdir/health，共二十二个操作。HTTP cleanup 与密码／metadata 线程退出共同约束实例停止及 lease 的最终释放。

[身份服务集成测试](../../tests/integration/IdentityService_unittest.cc) 使用真实 SQLite／crypto，覆盖注册邀请回滚、空间隔离、分页绑定、快照版本复核、Session上限及已提交但确认未知时不返回秘密；[身份执行器测试](../../tests/integration/IdentityExecutor_unittest.cc) 覆盖真实登录／邀请／注册／读取、dummy验证、限流与容量、密码期间 metadata 可进展、取消和停止屏障、工厂失败。[限流测试](tests/AuthenticationLimiter_unittest.cc) 使用受控时间验证有界准入与来源规范化。没有性能或物理机持久性结论。


[HTTP 身份集成测试](../../tests/integration/HttpIdentity_unittest.cc) 从真实初始化实例及排他 lease 出发，使用生产 HTTP、SQLite、crypto 和 worker，验证两用户邀请注册／登录、私有空间隔离、公共目录协作、游标绑定、邀请消费回滚／耗尽／撤销、登出及重启后的 Session 状态。实际32条已提交 Session 的上限响应是409，来源／登录名限流是429，伪造转发头不能重置来源预算。

受控密码 gate 验证真实 HTTP 已关闭时密码 worker 仍阻止实例进入 Stopped，锁保持到对象析构；取消的验证不会生成 Session。测试用事务包装器模拟“真实提交成功但确认丢失”，验证 HTTP 返回 COMMIT_OUTCOME_UNKNOWN 且不返回 token、不重复签发。该故障注入不是硬件掉电证据。实例服务测试也验证密码资源启动失败不会绑定端口。


账户管理通过同一 IdentityDispatcher 与 IdentityExecutor 提供，简单操作在metadata worker临时构造 [AccountService](../application/identity/AccountService.h)，无需额外资源工厂或线程。改密共享8个密码工作流与128个总任务上限，单个任务在password worker顺序验证和哈希；旧密码错误不执行hash，取消后不进入最终事务。秘密输入在密码阶段用完后销毁。改密不触发登录／注册的周期清理事务。

账户方法通过生产实例现有identityDispatcher接入九个账户HTTP操作，无额外构造参数。IdentityExecutor集成测试还覆盖所有账户调度重载、真实改密与重新登录、共享限流、错误旧密码不哈希、密码gate期间metadata进展和版本冲突、取消／停止等待worker与恰一次完成。

真实HTTP身份集成还覆盖改密计算期间资料更新使最终提交返回412、改密关停等待密码worker且持续持有lease，以及密码实际提交但确认未知时撤销旧Session并只递增一次版本。实际CLI进程验证账户更新、管理额度与mkdir先后顺序、启禁／重新登录及本地凭据处理。

设置 `ANOF_SHUTDOWN_TRACE` 可按需输出关停阶段的双时钟记录，包括 HTTP stop 回调、worker 最后通知、
退出 future 的确认及实例停止条件。默认关闭；不记录业务数据，不改变 grace 或退出条件。
诊断字段和只读进程快照的用法见 [诊断工具说明](../../tests/diagnostics/README.md)。
