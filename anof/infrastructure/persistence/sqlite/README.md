# SQLite 元数据基础设施

SQLite 内部代码负责连接、语句、当前 schema、持久化值和仓储适配。SQL、SQLite 句柄及错误码只在本目录及局部测试使用，不进入业务端口、HTTP 或 CLI。

当前 schema 3、身份与目录仓储、读快照及写事务均已接通，可构建 `anof_infrastructure`；没有开发版本升级链。业务 Store 与启动维护共用数据库引导校验；事务化 Bootstrap、文件初始化及 M2 启动维护已接通，数据库打开本身仍不能代替持锁、配置与格式检查。

## 建库与结构验证

[SqliteSchema.h](SqliteSchema.h) 提供 SQLite 内部接口：

| 接口 | 行为 |
|---|---|
| `createSqliteDatabase(path, options)` | 以 O_EXCL／O_NOFOLLOW 独占创建 0600 主库，再建立当前 schema；已有文件（含空文件）或孤立 WAL／SHM／journal 返回 Conflict |
| `createSqliteSchema(connection)` | 无活动事务时取得写事务；空 schema 原子建库，已有当前库只校验，旧／未知／损坏结构拒绝 |
| `verifySqliteSchema(connection)` | 同一读事务内核验设置、实际对象定义、版本记录、外键和完整性，返回前结束读事务 |
| `openSqliteDatabase(path, options)` | 不带 CREATE 打开已有库，不改变持久日志模式，完成 schema 校验后返回内部连接 |

[当前 DDL](schema/current.sql) 是完整建库定义，CMake 嵌入 [SqliteSchemaSql.h.in](SqliteSchemaSql.h.in)，运行时无需源码文件。`schema_info` 固定 singleton=1，存版本 3、完整定义和建库 UTC 微秒时间；没有 M1 兼容建库、自动迁移或重置路径。

建库 DDL 和版本记录在一个 BEGIN IMMEDIATE 事务内提交。重复调用同连接建库接口只核验已有当前库；路径建库接口始终要求文件不存在。未知库不清空；独占创建后遇到权限、DDL 或提交失败保留文件现场，不自动删除。初始化适配器负责安全祖先目录、实例 lease 和文件发布同步；低层路径接口不代替这些检查。

校验将编译内置 DDL 在临时内存库生成参考对象目录，逐个比较当前库表／索引／触发器的 type、name、tbl_name、SQL，核对对象数量，拒绝额外用户对象。记录中的 definition 只比较、不执行，不能靠保留正确版本记录掩盖修改 CHECK、替换唯一索引或删除触发器。此策略要求当前定义精确匹配；不接受语义近似的手工改写。SQLite 内部自动索引由对应表定义和完整性检查覆盖。

PRAGMA foreign_key_check 和 integrity_check(1) 检查实际数据；后者在 SQLite 内比较唯一 ok 结果，不回显损坏行内容。返回诊断数量有界，扫描耗时随数据库规模增长。校验不代替业务授权，不校验对象文件，不证明断电耐久性。

## schema 3 数据边界

| 表 | 约束 |
|---|---|
| users | 规范 UUID、唯一小写 login、UTF-8 显示名、受限 Argon2id 编码、角色／状态／epoch、密码版本／用户 revision、UTC 时间；至多一个管理员，管理员不能禁用，身份和登录名不可改 |
| sessions | 32 字节摘要主键、用户外键、epoch、created_at／expires_at、revoked；credential_mode 仅 bearer，结束时间大于创建时间 |
| invites | UUID、唯一摘要、启用管理员创建者、至多 30 天期限、1..10000 次上限、合法使用次数、不可恢复的撤销；按 created_at／id 索引 |
| instance_settings | 单行实例／操作身份、初始化时间、1..10000 人数上限、私有／贡献默认额度、revision；初始化身份不可改 |
| spaces／nodes／blobs | 私有所有者与唯一公共空间、延迟空间／根外键、活动同名唯一、目录／文件形状、不可删除根、Blob 大小与摘要 |
| space_quotas／public_contribution_quotas | 受检 used＋reserved 不超过 limit，全部数值非负整数，revision 至少 1；贡献仅对应公共空间 |
| audit_events | 创建目录成功事件的 actor、space、node、parent、request、时间 |
| identity_audit_events | 按 action 强制 actor／subject／invite／instance／quota 字段组合，相关外键和独立 sequence；不保存密码、Session 摘要或凭据 |

目录名称保持 BINARY 字节语义，活动文件／目录共用命名空间。子列表索引为 space、parent、name、id（仅活动节点），不依赖 OFFSET；仓储仍需验证活动祖先与可见性。身份条件写和原子邀请消费由下述身份仓储完成；人数／Session 上限和审计编排由应用在同一写事务执行。额度条件写与计费使用受检算术，并在实际变更时增加 revision。

“至多一个”约束不等于生产需要的“恰好一个”。当前建库不写 settings、用户、根、额度、Session、邀请或审计。未引导库允许通过本层结构检查；完整实例的 settings／管理员／空间／根／额度一致性和 pending／格式检查不属于这个通过结论。

## CHECK 纯验证函数

[SqliteFunctions](SqliteFunctions.h) 在每个模块连接注册 deterministic／innocuous 的 `anof_uuid`、`anof_login`、`anof_display_name`、`anof_password_hash`。函数先检查类型和字节上限，再复用领域值解析；不访问文件／数据库，不运行密码算法，不输出输入内容。C++ 异常不会穿过 SQLite 回调边界。

password_hash 限为 Argon2id v19，schema 硬边界为 8..262144 KiB、1..10 次、parallelism=1，规范 16 字节盐／32 字节输出，最多 512 字节。读取及真正验证密码仍必须使用显式的适配预算，schema 上限不是生产推荐参数。明文、畸形、超预算和不同输出长度不能通过 CHECK。

这些函数是当前 schema 的执行依赖。未注册函数的原生 sqlite3 工具不能直接执行相关写入或完整 CHECK 扫描；不能为了离线修改而关闭约束。直接 SQL 不属于应用或运维业务接口。

## 连接、资源及错误

[SqliteConnection](SqliteConnection.h) 保持 WAL、foreign_keys=ON、synchronous=FULL，busyTimeoutMs 为 0..5000（默认 100）。连接单线程使用、不可跨 worker；语句先于连接销毁。写事务用 BEGIN IMMEDIATE，未提交析构回滚；无法证明回滚的提交失败保留 CommitOutcomeUnknown 并限制连接复用。

Inspect 打开模式只设置连接本地的外键和同步参数，检查既有 WAL，不执行 journal_mode=WAL 转换。其他原始模式用于显式建库及底层测试，仍会配置 WAL，不能拿它们替代 verify-only 启动。正常 WAL 打开／关闭可能处理伴随文件，所以验证并不承诺文件系统绝对只读。

路径建库返回 Conflict（已存在）、NotFound（缺失父目录）、StorageFull（ENOSPC／EDQUOT）、IntegrityError（创建后私有属性不符）；参数错误 InvalidArgument，其他系统故障 InternalError。schema 标记缺失、旧／未来版本、定义或数据损坏为 IntegrityError；Inspect 遇到非 WAL 模式沿用设置不符的 InternalError。其他 SQLite Busy／Locked、Full、提交未知等分类由 [SqliteError](SqliteError.h) 保留，不猜测安全重试。

[SqliteStatement](SqliteStatement.h) 使用绑定参数和复制语义，保留文本／BLOB 长度、NULL 和空值区别，拒绝类型混读和重复 step 写入。[SqliteValue](SqliteValue.h) 校验领域 ID、计量、摘要及 UTC 微秒表示。业务仓储接口不暴露 execute(sql)。

## 身份仓储与成功审计

`anof_sqlite_identity` 包含六类内部适配器，均借用同一个 [SqliteTransactionContext](SqliteTransactionContext.h)，不自行开启或提交事务。上下文必须先成功 begin，并在所有仓储销毁后才销毁；连接不得跨线程。事务关闭后，旧仓储不能借后续事务继续工作。失败会记录在上下文中，commit 不会提交此前的部分写入；未提交析构回滚。

| 实现 | 当前行为 |
|---|---|
| [SqliteAuthorizationReader](SqliteAuthorizationReader.h) | 按 SessionCredential 查 Bearer 摘要，验证撤销、期限、启用状态和 epoch；损坏字段返回 IntegrityError，普通失效返回 Unauthenticated |
| [SqliteUserRepository](SqliteUserRepository.h) | 用户／密码快照读取、UserId seek 分页、总人数、新用户插入；规范登录名冲突返回 LoginConflict；显示名、启用状态和密码按预期 revision／version／epoch 条件更新 |
| [SqliteSessionRepository](SqliteSessionRepository.h) | 当前 epoch 的有效 Session 计数、插入、绑定用户撤销、全部撤销；按期限／摘要顺序分批删除失效记录，每次最多 200 条 |
| [SqliteInviteRepository](SqliteInviteRepository.h) | 创建、读取、created_at／id seek 分页、原子消费剩余次数、幂等撤销；不存在、过期、撤销或耗尽的消费统一为 InviteUnavailable |
| [SqliteSettingsRepository](SqliteSettingsRepository.h) | 可选设置读取、仅一次引导插入；不覆盖已有设置 |
| [SqliteIdentityAuditRepository](SqliteIdentityAuditRepository.h) | 将全部 typed IdentityEvent 变体映射到独立身份审计表，与调用方业务写入共同提交或回滚 |

UserRepository 构造还须显式传入 PasswordHashLimits，接纳范围为 8..262144 KiB、1..10 次、parallelism=1。密码快照读取受该预算和 512 字节上限约束，不运行密码算法；返回 move-only EncodedPasswordHash。仓储不保留调用方借用视图。SQLite 内部仍可能复制绑定及查询数据，这不是数据库缓冲区秘密擦除保证。

`list(PageSize, optional<UserListPosition>)` 包含管理员和禁用用户，按规范 UserId 文本升序、严格晚于指定位置查询。只读取资料列，最多读取 size+1 行、返回 size 项；仅存在下一项时 next 为本页最后返回用户的位置。位置对应用户不存在也可继续 seek；空页和末页没有 next，不使用 OFFSET，不读取密码列。返回自有值，同一读快照内续页保持一致，跨快照分页不承诺并发注册下的完整遍历。

显示名和启用状态先检查预期 revision，再判断无变化；无变化保留 revision、updated_at 和 auth_epoch。管理员禁用保护也在 revision 比较之后。实际禁用增加 auth_epoch／revision，重新启用只增加 revision，旧 Session 不恢复。改密比较 enabled 和 password_version／auth_epoch／用户 revision，成功递增三项版本；相同密码编码仍算一次更新。版本溢出返回 IntegrityError，条件失配返回 PreconditionFailed。

账户实际更新在同一写事务内读取旧记录，以 `max(now, created_at, updated_at)` 写入并返回 updated_at；允许时间相等，不通过加一制造递增。无变化仍保留原时间。邀请首次撤销保存 `max(now, created_at)`，重复撤销返回 false 并保留首次时间。此规则只约束记录事件时间；认证、到期、保留期判断和审计观察时间仍使用调用方传入的原始 UTC，不对系统时钟作全局钳制。

Session 全部撤销和成功审计须由调用方显式加入同一事务；无变化时调用方跳过审计。显示名更新不撤销会话。改密最终认证应在自身撤销会话之前完成，事务内不执行密码计算。审计拒绝或忽略插入均使整个事务失败；未提交析构回滚。CommitOutcomeUnknown 不等于已回滚，事务结束且连接不可复用，不能自动重试业务。

调用方在 BEGIN IMMEDIATE 写事务内依次 count→检查上限→insert；第二连接须取得写锁后重新读取计数。仓储不管理人数策略、不淘汰 Session、不签发凭据，也不在事务内执行密码工作。邀请消费、注册相关写入和审计使用同一上下文，后续失败会回滚已消费次数。分页位置只控制排序，不代表权限。

身份审计仅存结构化身份、操作、请求 ID 和时间；不写密码、密码编码、token 或凭据摘要。初始化和注册事件允许无 actor，其余字段组合由具体变体和 schema 共同约束。成功事件的选择及无变化时不追加事件仍由应用负责。

这些实现已通过 MetadataTransaction 的完整访问器接通：authorization、users、sessions、invites、settings、spaces、nodes、quotas、directoryAudit 和 identityAudit。应用身份流程和 HTTP 接入不由仓储提供。

## Store、快照与事务生命周期

[openSqliteMetadataStore](SqliteMetadataStore.h) 接受路径和显式 `SqliteMetadataOptions{passwordHashLimits, connection}`。密码预算没有默认值，必须与密码验证适配器的接纳预算一致；非法预算返回 InvalidArgument。它打开已有 schema 3，验证 settings、恰一位启用管理员、恰一个公共空间、每用户私有空间／根、必需额度和合法根创建者，并按预算检查密码编码。空间根至少占用一个节点，公共根也占用管理员的一份贡献节点。未引导或不完整数据库返回 IntegrityError，既有数据不被补齐或重置。

此门禁不接收实例 lease 或配置，不能验证 pending、格式标记及其实例 ID 匹配，也不是初始化或 finish 工厂。schema 校验与引导门禁分别在自己的读事务内执行；路径和启动时排他所有权由装配方保证，正常业务不允许修改 schema。返回 Store 前所有校验事务已结束。

Store 及其快照／写事务必须在同一元数据线程使用；Store 比这些作用域活得久，仓储引用不能超过所属作用域。一个 Store 同时最多一个读快照或写事务，嵌套返回 InvalidArgument。独立 Store 各持连接；WAL 允许读快照存在时另一连接写入。写锁竞争按有限 busy 策略返回 Retryable。

- `beginRead()` 创建 [只读快照](SqliteMetadataReadSnapshot.h)。BEGIN 在第一次实际查询时建立 SQLite 读视图，随后相关读取保持一致。返回对象的动态类型只实现 Reader，不暴露可写 Repository；内部查询组合复用同一生产实现。连接同时启用 query_only，阻止读事务升级为写事务。
- 快照析构回滚读事务并恢复 query_only；清理失败会使连接不可继续使用。即使读取失败也必须结束快照。结果包含自有领域值，密码快照使用 move-only 编码持有者；密码计算前应销毁整个数据库快照。
- `beginWrite()` 使用 BEGIN IMMEDIATE。全部仓储和两类审计共享同一上下文；未提交析构回滚，commit 无论结果都结束本次作用域。已结束事务的仓储不能借后续事务继续访问；其析构也不会回滚新事务。无法证明回滚时保留 CommitOutcomeUnknown，不承诺安全重试。

## 空间、节点和额度

[SpaceRepository](SqliteSpaceRepository.h) 提供 typed 插入与读取。listAccessible 按 SpaceId 升序返回本人私有及公共两项；缺少必需空间或重复结构为 IntegrityError。`findPrivate(UserId)` 仅查询指定所有者的私有空间，包含禁用所有者；无匹配返回 nullopt，不回退到公共空间或 listAccessible。应用确认目标用户存在后，若必需私有空间缺失，应按 IntegrityError 处理。仓储查询不授予私有节点访问权。

[NodeRepository](SqliteNodeRepository.h) 的 insertRoot 固定空名、无父、目录类型和 revision=1，检查空间指定根及创建者归属；空间和根必须同事务完成。根插入不自行计费，初始化／注册调用方须在同一事务插入正确 used_nodes 的额度行，避免再 chargeUsed 重复计费。

find 返回活动节点自有元数据，目录使用 DirectoryContent，文件从 Blob 读取 size 和 32 字节 SHA-256；损坏或缺失内容为 IntegrityError。隐藏回收站祖先下的节点不可见。路径验证保留同空间、活动祖先、循环及最多 129 节点限制；目标文件作为父节点为 ParentNotDirectory。

listChildren 校验父路径，按名称 UTF-8 原始字节、NodeId 有界 seek，每批最多 200 项，额外读取一项判断 next；不使用 OFFSET。它不信任游标为授权凭证，应用仍须在同一快照认证并验证空间访问权。已有创建目录的同名唯一性、父 revision、目录深度和回滚行为保持不变。

`advanceRevision` 在同一写事务读取节点旧 revision、created_at 和 updated_at，保留预期 revision 检查，并以 `max(now, created_at, updated_at)` 保存 updated_at。时间回拨仍可推进 revision，时间相等合法；条件冲突、溢出和回滚语义保持不变。

[QuotaRepository](SqliteQuotaRepository.h) 提供 get、insert、setLimit 和 chargeUsed。插入 revision 固定为 1；空间和公共贡献额度共享受检逻辑。实际修改 limit 或 used 都增加 revision，无变化不增加。setLimit 首先核对 expected revision，过期为 PreconditionFailed，下调到 used＋reserved 以下为 QuotaExceeded；缺行、损坏或整数／revision 溢出为 IntegrityError。读、检查与条件写共享写事务，两类额度及审计可一起回滚。

## 构建与测试

基础目标 PUBLIC 依赖 anof_application，PRIVATE 依赖 SQLite::SQLite3；身份目标 PUBLIC 链接基础目标，PRIVATE 链接 SQLite::SQLite3。anof_infrastructure 私有链接身份目标，测试使用同一生产实现。

```sh
cmake --build cmake-build-debug --parallel 2 --target \
    anof_sqlite_foundation_unittest anof_sqlite_identity_unittest \
    anof_sqlite_transaction_unittest anof_sqlite_node_unittest \
    anof_sqlite_quota_audit_unittest anof_sqlite_acceptance_unittest \
    anof_sqlite_read_snapshot_unittest anof_sqlite_headers_check \
    anof_sqlite_foundation_headers_check anof_sqlite_identity_headers_check
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_sqlite_(foundation|identity|transaction|node|quota_audit|acceptance|read_snapshot)_unittest$' -j 1
```

Release 使用 cmake-build-release。各测试有有限超时；[22 个案例](tests/SqliteFoundation_unittest.cc) 使用真实小型临时数据库，保留设置、重开、绑定／值解码、DDL 失败回滚、目录根／外键、名称竞争、配额／Blob、独立写连接竞争、SQLITE_FULL 和提交未知分类覆盖，并验证身份字段／Session／邀请／设置／审计约束、实际定义篡改、CHECK 损坏、旧库无修改拒绝及独占建库。七个基础头文件独立编译，不需要仓储或 transport。

[29 个身份案例](tests/SqliteIdentity_unittest.cc) 覆盖读取所有权、事务失效、条件更新／溢出、Session 认证／清理、邀请消费／分页、设置和各类审计、业务与审计原子回滚、独立连接锁竞争后的登录名冲突及人数／Session 上限检查。另有八个身份内部头文件独立编译。测试的 count→insert 编排不等同于真实应用身份流程。

SQLITE_FULL 通过限制页数触发，提交／DDL 故障使用 authorizer；不填满磁盘，不模拟硬件掉电。未引导夹具的密码编码只用于结构验证，不能用于真实登录，也不是生产默认密码。

[22 个快照／接口案例](tests/SqliteReadSnapshot_unittest.cc) 覆盖只读动态类型、WAL 一致性、清理故障、完整事务访问器、空间／根原子创建、文件元数据、分页、额度 revision／竞争／溢出、身份审计回滚和生产 Store 门禁。真实 Blob 文件读写不在本模块；1 TiB 文件大小仅作合成数值测试。

事务、节点、额度／审计和故障专项分别保留 13、12、17、7 个案例，包含两连接竞争、WAL、进程中断和受控 VFS 故障。上述七组共 122 个案例；完整 SQLite 头文件目标检查 22 个头文件。局部数据库验证不等于硬件断电或身份端到端验收。

[SqliteTestMetadataBackend](tests/SqliteTestMetadataBackend.h) 及 [SqliteTestStore](tests/SqliteTestStore.h) 使用当前 schema 和显式小密码预算，通过内部连接装配支持未引导库；故障测试允许注入损坏或额外触发器，测试观察接口不执行生产启动门禁。公开打开工厂没有绕过开关，并另行验证拒绝未引导库。

`anof_sqlite_contract_unittest` 运行根目录维护的共享后端语义用例；模块局部测试不替代共享契约或上层业务验收。初始化与启动维护的专项覆盖见下节。

账户、节点及邀请的时间专项使用真实临时 SQLite，覆盖回拨／相等／前进、时间整数边界、返回值与持久化一致、连接关闭重开、无变化／陈旧条件及未提交回滚；邀请另验证首次／重复撤销和原始 UTC 到期判断。

账户专项还覆盖精确 used+reserved 下限、零值／INT64_MAX、缺失额度行及版本溢出；私有／公共空间与管理员／普通成员贡献分别验证。独立连接的写事务重叠时，竞争方先收到 Retryable，再显式开启新事务读取当前状态：计费先提交使旧调额 revision 失效；降额先提交使后续超额计费失败。测试不依赖 sleep 或无限重试。

用户分页与私有空间查询验证读快照固定、写事务读己之写、外部观察者仅见提交、提交或回滚后返回值仍自有。身份事务测试覆盖禁用／撤销／再启用、显示名保留会话、无变化不追加审计、审计失败和析构回滚，以及提交未知后的事务关闭和连接停用。这些是适配器组合验证，不包含账户 HTTP、密码 worker 或远程客户端验收。

## 事务化引导与启动维护

[makeSqliteInstanceBootstrap](SqliteInstanceBootstrap.h) 接受 `SqliteBootstrapOptions{passwordHashLimits, connection}`，返回 `InitResult<std::unique_ptr<InstanceBootstrap>>`。[makeSqliteInstanceMaintenance](SqliteInstanceMaintenance.h) 接受显式 `SqliteMaintenanceOptions{passwordHashLimits, connection}`，返回 `Result<std::unique_ptr<InstanceMaintenance>>`。两者构造均不操作实例，成功持有非空独占对象；不存在无参数兼容入口。密码编码读取预算须与密码验证适配器一致：8..262144 KiB、1..10 次、parallelism=1；这里仅解码，不计算密码。

`createCurrent(lease, TransferBootstrapSeed)` 要求真实 Linux lease、匹配的当前 pending、完整八键配置和安全的 uploads／objects／quarantine 目录，且格式文件、主库和所有 SQLite 伴随文件均不存在。主库在已验证 `.anof` 描述符上 O_EXCL／O_NOFOLLOW 独占创建为 0600，不接管既有空文件。直接创建 schema 3，再以 BEGIN IMMEDIATE 运行一个引导事务，同时插入显式 contentBudget、P=R=0、revision=1 的 storage_accounting 行：

- settings 记录实例／操作 ID、初始化时间、人数上限和默认额度；revision=1。
- 一位启用管理员，auth_epoch=0、password_version=1、revision=1，使用调用方提供的密码编码。
- 公共空间和管理员私有空间，以及各自空名目录根；根 revision=1。
- 两条空间额度和一条管理员公共贡献额度；每条 used_nodes=1、used_bytes=0、reserved=0、revision=1。根插入不重复扣费。
- 一条 instance_initialized 审计，与所有记录同事务提交；没有 Session、邀请或默认凭据。

schema 创建与业务引导是两个事务。任一业务写入或审计失败回滚整个引导；保留数据库、pending 和配置供检查。CommitOutcomeUnknown 表示无法证明提交或回滚，不自动重试。成功提交后检查完整初始状态，关闭维护连接，再同步数据库／残留安全伴随文件及目录项；文件同步结果不确定用 PublicationOutcomeUnknown 表示，不把主库 fsync 当作 WAL 提交的替代。

`verifyComplete(lease, identity)` 只接纳可信 pending、完整配置、当前 schema 和已提交引导，核对实例与操作 ID；格式文件可缺失或完全匹配。它要求精确初始状态和一次根计费，返回同一 InitReceipt，不重新哈希、不补行、不改密码或额度。重复验证不会追加审计。验证成功后，调用方仍须通过 [InitializationLease](../../system/README.md) 发布格式并清除 pending。所有 SQLite 连接、语句、事务在返回前结束，lease 继续由调用方持有。

普通 `InstanceMaintenance::verify` 首先拒绝任何类型的 initialization.pending，然后严格读取 format 3，校验 schema 3、SQLite 完整性／外键、settings 与标记 ID、唯一启用管理员和公共空间、每用户私有空间／根、必需额度、合法根创建者和初始化审计。所有用户密码编码须符合显式预算。核验三个内容目录的权限、所有权和同设备属性，检查物理计量单例存在及受检数值。允许合法待恢复上传和超预算状态通过结构维护，由根侧恢复协调器处理文件归属及对账；不因等待恢复而删除或重置记录。允许后续合法用户、节点、额度和 revision 变化，不要求运行后的库仍为初始计数。配置由 prepareInstance 在维护前加载并复验；单独调用 verify 不代替配置检查。

安全数据库文件工具保留主库 fd 到 SQLite 连接关闭后，避免提前 close 干扰 POSIX 锁；打开前后及关闭后复验文件身份和 WAL／SHM，拒绝 journal、符号链接和不安全权限。验证使用已有主库、WAL 模式，不创建缺失主库、不转换旧格式、不修复数据。SQLite 正常读连接可能执行 WAL 恢复／伴随文件管理，不承诺文件系统绝对只读。

初始化错误带具体 InitStage：已有数据库／伴随文件 AlreadyExists，缺失可信文件或未引导库 IncompleteInitialization，不支持的版本 UnsupportedFormat，结构或 ID 不一致 IntegrityError，数据库忙 Busy。普通维护保留业务错误枚举：pending、不支持版本、不完整引导或 ID 不匹配为 IntegrityError，缺失必需文件 NotFound，SQLite 锁竞争 Retryable。错误不包含密码编码或 SQL 诊断内容。

`anof_sqlite_instance_bootstrap_unittest` 的 [13 个案例](tests/SqliteInstanceBootstrap_unittest.cc) 覆盖真实初始化、收尾和 prepareInstance、精确计费、重复调用、读取预算、权限、已有文件保留、未引导库、身份／配置／版本不一致、缺行及错误审计、普通运行变更、pending 类型、审计回滚、真实 WAL 同步故障和提交前后子进程退出。另有维护 11 个、准备流程 8 个案例，保留 WAL、安全路径、资源释放及跨进程锁覆盖。故障钩子仅存在于内部测试构造，不暴露到公共端口；测试不证明硬件断电持久性。

```sh
cmake --build cmake-build-debug --target anof_sqlite_instance_bootstrap_unittest anof_sqlite_instance_maintenance_unittest anof_sqlite_instance_preparation_unittest anof_sqlite_read_snapshot_unittest anof_sqlite_headers_check --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_sqlite_(instance_bootstrap|instance_maintenance|instance_preparation|read_snapshot)_unittest$' -j 1
```

Release 使用现有 cmake-build-release。根 runtime／CLI 需显式传入维护预算并编排初始化；局部成功不表示完整 HTTP 身份流程或 CLI 已接通。

## 并发与初始化完成边界

[SqliteM2Boundaries_unittest.cc](tests/SqliteM2Boundaries_unittest.cc) 注册为 `anof_sqlite_m2_boundaries_unittest`，含 8 个案例。两个执行线程分别构造和销毁自己的连接，以限时同步点制造写事务重叠，验证规范化登录名、最后一次邀请、最后一个用户名额及 Session 名额。竞争方先确认 Retryable，再在获知前一事务已结束后显式开始新事务并重新读取；不在适配器内自动重试。邀请注册回滚案例检查消费次数、用户和审计一起回滚，随后另一连接可成功消费。人数／Session 限制检查属于测试调用方编排，不表示仓储自行实施应用策略。

同一专项还组合真实文件初始化和 SQLite Bootstrap，在 pending 发布、配置发布、格式 rename 前后、pending unlink 前后六处直接退出子进程。重新打开后根据当前文件和数据库证据区分不可收尾的半成品、可验证收尾的已提交引导和已经完成的实例，核对稳定锁 inode、审计与根计费。并发创建只有一个成功；finish 持锁期间，其他 finish 返回 Busy，普通锁获取返回 Conflict，成功清除 pending 也不会提前释放 lease。

`anof_sqlite_instance_bootstrap_unittest` 的 WAL 同步故障检查还会在 CommitOutcomeUnknown 后重新读取已提交证据，确认不能再次 createCurrent，也不能凭前次错误推断回滚或自动重做引导。

同步点与 pipe 等待上限 5 秒，子进程 alarm 为 8 秒，专项 CTest 上限 45 秒。失败时回收任务自有子进程；没有 sleep 竞速、脱离线程或无限重试。数据仅为少量临时用户、Session 和 KiB/MiB 级数据库，串行 CTest；进程退出不等于硬件断电，也不证明 HTTP 身份编排已验收。

```sh
cmake --build cmake-build-debug --target anof_sqlite_m2_boundaries_unittest anof_sqlite_instance_bootstrap_unittest --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_sqlite_(m2_boundaries|instance_bootstrap)_unittest$' -j 1
```

Release 使用现有 cmake-build-release 和相同目标。

## 独立传输仓储

生产和测试统一使用 [当前 schema 3](schema/current.sql)，由 [SqliteSchema](SqliteSchema.h) 创建和校验，没有候选结构分支或升级接口。真实临时数据库工厂见 [SqliteTestTransferBackend.cc](tests/SqliteTestTransferBackend.cc)。[openSqliteTransferMetadataStore](SqliteMetadataStore.h) 返回类型化 TransferMetadataStore，与基础 opener 共用当前 schema 及已引导状态校验；调用方不需要 downcast。

`MetadataStoreImpl` 同时实现基础及传输 Store；两类快照和事务共享连接及禁止嵌套规则。传输读取暴露 Upload、Blob、Storage Reader，写事务同时提供 FileNodeRepository、ReservationQuotaRepository 和 TransferAuditRepository。仓储异常沿用事务失败与提交未知分类，不独立提交，也不执行文件操作或恢复补偿。

- Upload 持久化不可变原始参数和可变目标，按状态／generation／offset 条件替换。取消不可逆，覆盖 COMMITTING；实际变化推进 generation，重复取消无变化。
- 按 created_at／id seek 分页与扫描，每页最多读取 size+1 行。创建原子检查每用户／全局活动上限 8／64、保留上限 512／4096；不驱逐旧记录。到期扫描排除 COMMITTING，保留删除要求终态、无 cleanup_pending 且超过七天。
- Blob 只插入不可变 AVAILABLE 描述符，存储键由 ID 派生。文件节点引用既有 Blob，复验父目录、空间与唯一名称；父 revision 由调用方显式推进。
- 逻辑预留按范围提供 reserve／commitReservation／releaseReservation，覆盖空间及公共贡献。物理计量独立保存 budget、P、R 和 revision；发布将 R 转入 P，清理释放 R。算术受检，超预算状态仍可读取和清理，禁止新预留。
- typed 传输审计与节点、额度及计量共用事务。文件发布和 SQLite 提交不构成跨介质原子事务；提交未知时须由调用方销毁旧 Store、通过新连接读取权威证据。

`anof_sqlite_transfer_contract_unittest` 运行根共享的三个真实后端契约；`anof_sqlite_transfer_unittest` 覆盖容量、取消、保留期、快照、发布计量、审计失败回滚、独立连接竞争及提交未知。局部适配器测试不表示恢复编排或上传端到端验收完成。
