# 应用公共端口

本目录定义业务与数据库、crypto、调度和实例维护之间的 C++17 接口。接口不依赖 SQLite、HTTP、JSON 或 muduox；仓储方法接受领域值，不接受 SQL。纯虚接口描述适配器必须满足的行为，本身不提供后端实现。

## 元数据边界

[MetadataStore.h](MetadataStore.h) 分别开启只读 `MetadataReadSnapshot` 和可写 `MetadataTransaction`。同一 store 同时只能存在一个快照或事务；对象及其仓储在 metadata worker 使用，不跨线程。返回值自有，仓储引用只在其所属快照或事务存活期间有效。store 必须最后销毁。读快照只暴露 Reader；写事务提供 Repository 与两类审计，共享一个事务，仓储不能自行提交。写事务未提交析构回滚；提交结果未知不能视为回滚，也不能自动重试。

- `AuthorizationReader` 根据摘要、凭据模式、期限、撤销状态、用户启用状态和 epoch 检查 Session。业务必须在同一快照检查可见性；写用例在修改与提交前复验。
- `UserReader::list` 返回按规范 UserId 升序的有界资料页，包含管理员与禁用账户，严格晚于给定位置；不读取密码快照，不携带授权或跨页快照。授权由应用在所属快照或事务中完成。
- `UserRepository` 的登录名冲突为 `LoginConflict`。条件写失败为 `PreconditionFailed`。替换密码要求用户启用且密码版本、epoch、用户 revision 均匹配，然后递增三者。禁用管理员被拒绝；禁用实际发生时递增 epoch/revision，再启用只增 revision。撤销 Session 和审计由应用在同事务完成；人数检查与插入也必须在同一写事务。
- `SessionRepository` 的活跃计数不含到期、撤销或 epoch 失效项。应用在插入事务检查每用户最多 32 个活跃会话，超限为 `SessionLimitReached`。清理按 expires_at/digest 排序、每批最多 200 项。撤销返回实际变更标志，避免重复审计。
- `InviteRepository` 的消费必须原子递增次数；未知、到期、撤销、耗尽统一为 `InviteUnavailable`，注册其余步骤失败时一并回滚。列表不包含摘要或明文；撤销不存在项为 `NotFound`，已撤销项返回 false。
- `SpaceReader::findPrivate` 按用户查私有空间，无匹配返回空值，不执行授权；应用确认用户存在后将必需空间缺失视为 `IntegrityError`。`listAccessible` 返回本人私有空间和公共空间，按 SpaceId 排序；必需空间缺失为 `IntegrityError`。`NodeReader` 查询前需检查父路径及可见性，children 按 UTF-8 名称字节及 NodeId 排序。`Page` 至多返回请求条数，next 表示仍有后续项，不代表跨页快照。
- `QuotaRepository` 以 expected revision 修改额度；下调不得低于 used+reserved，否则 `QuotaExceeded`。实际额度或占用变更递增 revision，溢出整体失败。根插入、空间、初始额度由同一事务完成；根初始 revision 为 1。
- `SettingsRepository::insertBootstrap` 仅用于首次引导。目录审计和身份审计分开访问，身份事件使用封闭 variant，不接受任意 action/JSON，不包含密码、token 或摘要。

## 秘密与调度

[PasswordHasher.h](PasswordHasher.h) 只在独立密码 worker 执行，调用前释放数据库快照/事务。`EncodedPasswordHash` 的结构校验要求调用者提供明确的参数预算；它不执行密码验证。`CredentialIssuer` 返回只移动的秘密明文及独立摘要，随机生成在 worker 或离线初始化执行。邀请摘要与 Session 摘要是不同类型。失败或不确定提交不得返回新秘密。

[TaskDispatcher.h](TaskDispatcher.h) 定义任务句柄、接纳/拒绝和完成类型。`TaskSubmission` 同步拒绝不回调；接受后恰一次延后完成，由所属 I/O loop 消费。RateLimited 表示速率拒绝（429），区别于 QueueFull／Stopping（503）。取消不强制中断正在执行的事务，销毁句柄不隐式取消。多阶段执行仍只有一个任务身份，不能持事务等待另一 worker。

[IdentityDispatcher.h](IdentityDispatcher.h) 提供注册、登录、登出、当前用户、邀请、空间/节点查询及账户管理的类型化重载；实现位于 runtime，应用编排见 [IdentityService](../identity/IdentityService.h)。匿名命令携带 RequestId，鉴权命令使用 [RequestContext](../RequestContext.h)。注册／登录还携带仅由可信 socket peer 构造的 AuthenticationSource；runtime 用于限流，不信任客户端转发头。秘密命令和返回值只能移动。游标结构校验在 transport；命令携带的用户/空间/父目录绑定由应用在同一读快照内、授权和可见性检查之后核实。仓储 seek 位置不携带权限；身份 dispatcher 的分页结果由应用加入已核实的 InviteCursor/ChildrenCursor 绑定，供 transport 编码。

`CreateDirectoryDispatcher` 使用同一 TaskSubmission/TaskHandle；其完成结构额外保留 `CreateDirectoryAttempt`，用于提交未知的目录诊断。它不是身份调度的包装。

## 实例初始化

[InstanceInitialization.h](InstanceInitialization.h) 将文件/锁维护与数据库引导分开。`InitializationLease` 持有同一稳定实例锁，析构仅释放资源，不清空半成品；借用的 InstanceLease 不得超过它的寿命。

`create` 只接纳新 .anof，持久化 pending；配置、格式发布和 pending 清理各自报告结果。发布不覆盖已有标记；匹配的已发布标记允许验证 no-op。`createCurrent` 只接受不存在的数据库；`verifyComplete` 只核实当前版本已提交的引导与身份，不补数据或重设密码。维护连接在返回前关闭。

[Initialization.h](../maintenance/Initialization.h) 定义 schema/format 版本 2、类型化输入及独立 `InitResult`。数据库提交未知与文件发布未知是不同错误。`validateBootstrapSeed` 检查空间/根 ID 不冲突、密码编码未被移动清空及三组根节点额度；不代替文件状态、数据库不变量或持久化校验。维护端口自身不编排用例；[InitializeInstance / FinishInitialization](../maintenance/InitializeInstance.h) 提供对应应用编排。前者额外接收可信入口生成的 RequestId，后者只接收 root，不重新生成身份。

## 验证入口

`anof_contract_headers_check` 将 core/domain/application 每个公开头文件独立编译，无具体后端依赖。`anof_initialization_values_unittest` 检查根额度、引导 ID、秘密所有权及初始化错误分类；`anof_identity_values_unittest` 检查身份文本和秘密格式边界。头文件/值测试不证明具体数据库适配、密码算法或 HTTP 端点正确。

账户命令见 [AccountCommands.h](../identity/AccountCommands.h)。AccountCondition 为用户条件与额度条件的封闭variant，保留资源绑定；缺失或非法HTTP头在transport拒绝，合法但目标不匹配由应用返回PreconditionFailed。改密任务在读取真实身份之后才检查来源／登录名限流，已接纳的任务可通过完成结果返回 `ErrorCode::RateLimited`；与接纳前 `SubmitError::RateLimited` 同属速率拒绝，不混同Retryable或QueueFull。
