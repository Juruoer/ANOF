# SQLite 元数据基础设施

当前已实现连接和语句 RAII、持久化值读取、基础错误分类、最小 schema 和版本化迁移，
以及业务事务适配、身份验证、空间、节点、配额和审计仓储。
本文面向 ANOF 的维护和二次开发人员，说明现有能力、数据约束与维护边界。

最小事务端口已全部接入，共享契约工厂使用真实临时数据库验证持久化行为。[创建目录应用](../../../application/drive/CreateDirectory.h)通过这些端口访问元数据；局部测试不代表完整 M1 或断电持久性验收。

## 1. 模块结构与依赖

| 文件 | 职责 |
|------|------|
| [SqliteMetadataStore.h](SqliteMetadataStore.h) | 只暴露领域端口的后端工厂，管理连接和活动事务 |
| [SqliteMetadataTransaction.h](SqliteMetadataTransaction.h) | 事务内仓储所有权、提交与析构回滚 |
| [SqliteTransactionContext.h](SqliteTransactionContext.h) | 事务独立生命周期、失败状态和连接共享 |
| [SqliteAuthorizationRepository.h](SqliteAuthorizationRepository.h) | 当前事务内的 Session 与账户复验 |
| [SqliteSpaceRepository.h](SqliteSpaceRepository.h) | 空间读取及 kind／owner／UUID 校验 |
| [SqliteNodeRepository.h](SqliteNodeRepository.h) | 有界活动路径读取、目录插入及 revision 条件递增 |
| [SqliteQuotaRepository.h](SqliteQuotaRepository.h) | 空间／公共贡献者的原子计费，复用领域受检算术 |
| [SqliteAuditRepository.h](SqliteAuditRepository.h) | 创建目录审计与资源变更共同提交 |
| [SqliteOptions.h](SqliteOptions.h) | 无 SQLite 句柄的连接配置值 |
| [SqliteConnection.h](SqliteConnection.h) | 连接所有权、有限 busy 等待、设置核验、底层事务控制和连接失效状态 |
| [SqliteStatement.h](SqliteStatement.h) | 单条语句 RAII、带长度的参数绑定、严格存储类型读取 |
| [SqliteValue.h](SqliteValue.h) | UUID、计量、时间、摘要、布尔和枚举的持久化读取校验 |
| [SqliteError.h](SqliteError.h) | 稳定错误分类及内部操作阶段／SQLite 扩展码诊断 |
| [SqliteMigration.h](SqliteMigration.h) | 显式迁移、已有数据库打开和 schema 检查 |
| [0001_create_directory.sql](migrations/0001_create_directory.sql) | 最小 schema 的唯一 SQL 来源 |
| [SqliteFoundation_unittest.cc](tests/SqliteFoundation_unittest.cc) | 使用真实临时数据库的基础设施测试 |
| [SqliteMetadataTransaction_unittest.cc](tests/SqliteMetadataTransaction_unittest.cc) | 事务生命周期、身份／空间读取及隔离验证 |
| [SqliteNodeRepository_unittest.cc](tests/SqliteNodeRepository_unittest.cc) | 节点规则、坏树、名称竞争及 revision 边界 |
| [SqliteTestMetadataBackend.h](tests/SqliteTestMetadataBackend.h) | 仅测试使用的共享契约后端、seed 和提交后观察 |
| [SqliteQuotaAudit_unittest.cc](tests/SqliteQuotaAudit_unittest.cc) | 计费边界、审计原子性、额度竞争与 fixture 验证 |
| [SqliteAcceptance_unittest.cc](tests/SqliteAcceptance_unittest.cc) | WAL 快照／checkpoint、锁与迁移竞争、I/O 故障传播 |
| [SqliteFaultVfs.h](tests/SqliteFaultVfs.h) | 仅测试使用的单次 WAL 写入／同步失败注入 |

内部命名空间为 `anof::infrastructure::sqlite`。`anof_infrastructure` 静态目标 PUBLIC 链接 `anof_application`，
PRIVATE 链接 `SQLite::SQLite3` 和格式解析所需的 `nlohmann_json::nlohmann_json`。SQL、连接句柄和任意 SQL 执行能力只用于本模块内部，不属于
[应用端口](../../../application/ports/)。模块不依赖 HTTP、event loop 或 muduox，也不创建工作线程。

每个连接仅由一个线程使用，连接必须比借用它的语句活得久。语句不可复制，析构 finalize，绑定文本和 BLOB 使用复制语义；
空文本与空 BLOB 保持各自类型，不转换为 NULL。语句完成或失败后不能再次 step，避免 SQLite 的隐式重新执行行为导致重复写入。
读取接口只在当前行有效，拒绝 NULL／类型不匹配和越界列号，不将文本或 REAL 静默转换成整数。

## 2. 连接与维护入口

`openSqliteMetadataStore(path, options)` 返回 `Result<std::unique_ptr<MetadataStore>>`，其公开头文件不暴露 SQLite 句柄。
它通过内部 `openSqliteDatabase()` 打开并验证已有数据库，不创建或升级 schema。
`SqliteConnection::open()` 是更底层的内部入口，只配置连接，不检查 schema。不存在、空路径、内存数据库和含 NUL 的路径不会被当作正常业务数据库使用。

`SqliteOptions::busyTimeoutMs` 默认为 100 ms，接受 0～5000 ms；0 表示锁竞争立即返回。
连接启用私有缓存，设置并读回 WAL、`foreign_keys=ON` 和 `synchronous=FULL`。不满足这些设置时返回失败。
每次底层 beginWrite 也检查设置，拒绝在被关闭外键等状态下启动写事务。
数据库必须位于 Linux 本地文件系统；调用方负责路径、目录权限及实例锁，适配器不创建父目录。

`migrateSqliteDatabase(path, options)` 显式创建或检查数据库。`applySqliteMigrations(connection)` 是内部的同连接维护入口，
拒绝嵌套事务。迁移使用 `BEGIN IMMEDIATE`，在锁内读取历史；只有不存在用户 schema 对象的空库可以应用首个迁移。
未知已有数据库不会被覆盖。DDL 和迁移记录在同一事务内提交，中途失败或异常退出回滚。

迁移文本通过 CMake 嵌入二进制，运行时不依赖源码 SQL 的路径。SQL 文件变化会触发 CMake 重新配置。
`schema_migrations` 保存版本、文件名、确切 SQL 文本及 UTC 微秒时间，重复运行逐字核对并保留原数据。
历史缺失／内容不符、必需表／索引／触发器缺失或外键损坏都会拒绝使用；未知更高版本拒绝打开，不自动降级。

版本校验和外键检查在同一读快照中进行。当前 schema 检查核验必需对象的名称和类型，未比较对象的完整 DDL，
也不替代完整性检查、FSCK 或防篡改认证。维护入口须在无并行业务连接时调用；生产启动协调与备份流程尚未接入。
已应用迁移不得原地修改，维护生产数据时须通过新迁移、备份及恢复流程处理版本变化。

## 3. 持久化编码与表结构

UUID 使用规范小写、带连字符的非 nil `TEXT COLLATE BINARY`；摘要使用 32 字节 BLOB。
时间为有符号 Unix epoch UTC 微秒；字节数、节点数和 auth_epoch 为 0～INT64_MAX，revision 为 1～INT64_MAX。
枚举使用明确的整数映射，布尔限定为 0/1。普通表通过 `typeof`、范围、长度及格式 CHECK 约束数据。
读取非法 UUID、计量、摘要、枚举等返回 `IntegrityError`，不把输入解析错误当作调用方参数错误。

| 表 | 当前字段与约束 |
|----|----------------|
| `schema_migrations` | version、name、definition、applied_at；正整数版本主键 |
| `users` | id、role、enabled、auth_epoch；Member=0、Administrator=1，部分唯一索引限制至多一个管理员 |
| `sessions` | token_hash、user_id、auth_epoch、expires_at、revoked；32 字节摘要主键、用户外键；不存明文 token |
| `spaces` | id、kind、owner_id、root_node_id；Private=0 且 owner 非空，Public=1 且 owner 为空；私有 owner 唯一、至多一个公共空间 |
| `blobs` | id、size、sha256；非负大小、32 字节摘要；只包含元数据 |
| `nodes` | id、space_id、parent_id、name、kind、created_by、blob_id、revision、trashed、created_at、updated_at；Directory=0、File=1；目录无 Blob，文件必须引用 Blob |
| `space_quotas` | space_id 主键及外键，limit／used／reserved 的 bytes 和 nodes |
| `public_contribution_quotas` | space_id、user_id 联合主键及外键，同样的六个额度字段；触发器限定为公共空间 |
| `audit_events` | sequence、actor_id、space_id、node_id、parent_id、request_id、action、occurred_at；当前 action 仅为 directory_created |

外键不级联删除。审计中的用户和空间使用外键；节点、父节点及 request ID 是经过格式检查的历史标识，不以节点外键阻止后续删除。
request ID 不唯一，也不是幂等键。审计仓储每次 append 插入一条事件；一次创建恰有一条成功审计仍由应用编排保证，
仓储不自动去重或额外补写事件。

额度按两种强类型作用域分表，不使用可空 owner 组成唯一键。约束使用 `used <= limit` 及 `reserved <= limit - used`，
避免 CHECK 中先求和发生溢出。额度没有自动从 nodes 重算或自动计费的触发器；配额仓储在同一写事务内检查并更新。

账户尚无登录名／密码字段，Blob 尚无对象 key／发布状态，均需在对应能力接入时迁移补齐。
数据库允许空库和最小测试种子；至多一个管理员／公共空间的索引不等于生产实例初始化已经完成。

### 空间根、名称与目录树

nodes 的 `(space_id, id)` 唯一键支撑同空间父子复合外键。spaces 的 `(id, root_node_id)` 引用该键，
nodes.space_id 引用 spaces；循环引用使用延迟外键，可在同一事务先写空间再写根，但缺失根不能提交。

触发器限定根为活动目录、无父节点、空名称、无 Blob，禁止删除根或修改其结构属性。
节点 id／space／kind 和空间 id／kind／owner／root 在当前 schema 中不可更改。
普通节点拒绝自引用和文件父节点；跨空间父由复合外键阻止。

活动名称唯一索引覆盖 `(space_id, parent_id, name COLLATE BINARY)`，条件为 `trashed=0 AND parent_id IS NOT NULL`。
文件和目录共用命名空间，大小写按原始字节区分。活动无父节点的唯一性另设部分索引。
普通名称通过 SQL 校验 1～255 字节、分隔符、NUL 及 `.`／`..`；根独享空名称。
节点仓储复用领域规则验证完整 UTF-8 和 Unicode Cc，并在同一事务内检查祖先活动性、环与深度；
数据库局部约束不替代这些检查。

### 回收站表示的边界

`trashed` 只表示节点自身被标记。测试种子允许已标记节点保留 parent，其未标记后代仍不可见，
不能仅根据 `trashed=0` 判断可见性。schema 允许这种子树，节点仓储遍历祖先后决定是否可创建。
完整回收站条目、根脱离原位置、恢复及过期处理尚未实现。

## 4. 事务控制与错误处理

每个 store 拥有一个连接，所有事务内仓储共享该连接，仓储不自行提交。store 必须比事务活得久，
同一 store 只允许一个打开的事务，嵌套 beginWrite 返回 InvalidArgument；独立 store 之间通过真实 SQLite 锁协调。
每个事务拥有独立状态，commit 无论成功或失败都会关闭该事务；关闭后仓储方法和重复 commit 返回 InvalidArgument。
即使旧事务对象仍存活，也不会读写或回滚同连接上的后继事务。事务销毁后仓储引用失效，不得继续使用。

仓储返回失败后，事务被标记为失败，后续仓储操作和 commit 被拒绝，commit 尝试回滚并关闭事务；
空间不存在返回成功的空 optional，不会标记失败。检测到底层事务意外结束时也禁止隐式读写。
未提交事务析构自动回滚且不抛异常，回滚失败时连接不可复用。不支持跨线程或跨 worker 使用。

身份仓储按 32 字节摘要精确查询，不缓存 Principal。每次使用调用方传入的 UTC 时间检查 `expires_at > now`、
未撤销、账户启用及 auth_epoch 一致；正常的凭据缺失或失效统一返回 Unauthenticated。
SQL 先读取并严格解码各字段，损坏的类型、枚举、负 epoch 或断裂账户外键返回 IntegrityError。
同事务可在提交前用新的时间再次复验，覆盖事务执行期间自然到期；复验调用时机仍由应用编排负责。

空间仓储返回 id、kind、可空 owner 和 root ID；私有空间必须有 owner，公共空间必须无 owner。
它只返回持久化数据，是否允许当前用户访问该空间由领域策略判断，不内置管理员私有访问旁路。
祖先活动性、根结构遍历和创建深度属于节点仓储边界，空间读取不替代这些验证。

底层 beginWrite 使用 `BEGIN IMMEDIATE`，拒绝嵌套；commit 失败后尝试回滚，连接析构也回滚未完成事务。
若失败提交仍有活动事务且回滚成功，可确认未提交并按原因返回错误；不能确认时返回 `CommitOutcomeUnknown`，
并将连接标为不可复用。回滚失败同样使连接失效。仅看到 autocommit 恢复不能认定提交成功或安全重试。
这些判断依据 [SQLite 事务说明](https://www.sqlite.org/lang_transaction.html) 和
[autocommit 接口](https://www.sqlite.org/c3ref/get_autocommit.html)，不自动重试业务操作。

| 情况 | 当前基础分类 |
|------|--------------|
| 已确认未提交的 BUSY／LOCKED | Retryable |
| 已确认未提交的 FULL | StorageFull |
| 约束失败、CORRUPT、NOTADB、类型不符 | IntegrityError |
| 无法证明失败提交未生效 | CommitOutcomeUnknown |
| 无法打开、设置不符、未知版本或其他未分类故障 | InternalError |

唯一约束失败在基础层仍为 IntegrityError；节点插入仅在数据库报告 UNIQUE 失败且同事务确认存在活动同名条目时
返回 NameConflict。已有 UUID 在插入前独立检查并返回 IntegrityError；外键及其他约束失败也不映射为名称冲突。连接保留最近一次 SQLite 失败的操作阶段和扩展码，公共 Error 不携带 SQL、路径或参数。
当前没有应用日志接入；CreateDirectory 已提供可选的候选节点 ID／request ID 诊断输出，调用方负责保留并在后续 runtime 日志接入时记录。

## 5. 节点仓储行为

`findActiveDirectoryPath()` 按节点 ID 自目标向空间根读取，最多 129 个节点，并用 visited 集合检测环。
成功返回根到目标的有序路径；根深度为 0。目标不存在、目标属于其他空间或任一祖先被标记为回收站时返回 NotFound。
目标为文件时先检查祖先可见性，再返回 ParentNotDirectory；祖先为文件、断链、环、错误的空间根或超深路径返回 IntegrityError。
有效深度 128 的目录仍可读取，但不能再创建子目录。

`insertDirectory()` 自身验证名称、活动父路径和深度，即使调用方没有预先检查也不跳过这些约束。
文件和目录共用活动名称唯一性，回收站同名节点不占用原父目录的活动名称；名称保留原始字节，不 trim 或归一化。
成功结果的 revision 为 1，createdAt 与 updatedAt 均来自命令。插入只创建目录元数据，不隐式计费、递增父 revision 或写入审计。
createdBy 由可信应用从复验后的身份提供，仓储不根据参数重新推导权限。

`advanceRevision()` 在同一写事务中区分节点缺失、expected 不符及溢出，使用受检加法和带 expected revision 条件的 UPDATE，
同时更新 updated_at。插入和更新均核验实际影响行数，不能将被忽略的写入返回为成功。此接口不自行检查调用者权限或活动路径，
调用方负责资源校验。多个仓储操作的提交和回滚仍由所属事务统一负责。

## 6. 配额、审计与共享测试后端

`chargeUsed()` 将 SpaceQuota 映射到空间额度表，将 PublicContributionQuota 映射到公共贡献额度表，
缺失计费行、非法持久化数值及非公共空间的贡献行返回 IntegrityError。读取 limit／used／reserved 后调用领域
`chargeUsed()`，先验证已有占用，再受检计算两个维度的新占用；有效数值超限返回 QuotaExceeded，算术溢出返回 IntegrityError。
两个维度都成功后才一次更新 used_bytes／used_nodes，保留限额和预留不变，并核验影响行数。
检查与更新共享已取得写锁的事务，没有事务外额度缓存或每仓储独立提交。

创建公共目录应由应用分别计入公共空间和贡献者额度，管理员也不豁免。第二作用域失败会使整个事务只能回滚，
不会保留第一个作用域的计费。此接口没有上传预留、释放或 reserved 转 used 的功能。

`append()` 原样绑定 actor、space、node、parent、request 和事件时间，action 固定为 directory_created，核验插入一行。
用户／空间外键、数据库故障或被忽略的写入导致失败时，前面的计费、节点和 revision 一并回滚。
审计不自行验证 Session 或生成身份；这些信息由可信应用提供。节点标识允许保留历史引用，不能用 append 替代节点存在性验证。

测试目录的 `SqliteTestMetadataBackend` 实现共享 `makeTestMetadataBackend()` 工厂。每个后端拥有独立 `/tmp` 目录，
reset 在无连接时重建自己的数据库、执行迁移，再在一个事务内写入 seed；延迟外键允许节点种子按非拓扑顺序给出。
配额按 seed 明确写入，不从节点数量重新计算；Blob 只播种元数据，不创建内容文件。失败 seed 回滚，不残留半份业务数据。

每次 connect 返回独立 store。readNode、readQuota 和 readAudit 使用新的真实连接读取已提交状态，
readAudit 按 sequence 排序；这些观察接口要求没有活动写事务。调用方必须按事务、store、backend 的次序销毁，
reset 及观察接口的前置条件由测试调用方保证。清理仅涉及当前 fixture 目录。
测试接口及故障注入路径不链接到生产库，不扩展业务端口。

## 7. 构建与验证

从仓库根目录执行：

```sh
cmake -S . -B cmake-build-debug -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DANOF_BUILD_TESTS=ON
cmake --build cmake-build-debug --target anof_sqlite_foundation_unittest anof_sqlite_transaction_unittest anof_sqlite_node_unittest anof_sqlite_contract_unittest anof_sqlite_quota_audit_unittest anof_sqlite_acceptance_unittest anof_sqlite_instance_maintenance_unittest anof_sqlite_instance_preparation_unittest anof_sqlite_headers_check --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_sqlite_.*_unittest$' -j 1
```

`ANOF_BUILD_TESTS=ON` 注册八个 SQLite CTest，局部测试超时 30 秒，共享契约通过现有 helper 设置 60 秒超时。
SQLite 测试包含 82 个案例，其中共享契约为 7 个案例。`anof_sqlite_headers_check` 包含 15 个头文件的独立编译检查，其中包含实例维护工厂。
基础设施的 14 个案例覆盖：
连接设置、重开和重复迁移、严格绑定／解码、历史异常和未知数据库、迁移中途 DDL 失败回滚、根及外键、
活动名称、额度边界、真实连接竞争、析构回滚、真实 SQLITE_FULL、提交／回滚失败的保守分类和外键损坏检查。

事务与读取的 13 个案例覆盖：私有／公共／缺失空间、有效成员／管理员身份、失效凭据、摘要精确匹配、
到期边界复验、64 位 epoch、跨事务身份更新、嵌套拒绝、旧事务隔离、并发撤销竞争、同连接读取、
提交／析构回滚、坏数据、底层自动回滚、提交失败关闭及关闭后的配额／审计仓储隔离。

节点仓储的 11 个案例覆盖：目录快照与重开、插入／revision 整体回滚、UTF-8 和字节名称规则、同名文件／目录、
UUID 与外键错误、缺失／跨空间／文件父节点、回收站祖先、深度 128、坏树、revision 整数边界、双连接同名竞争及零行写入。
坏树和零行写入通过测试专用 SQL 构造，不改变生产迁移约束。

共享契约的 7 个案例覆盖：全部仓储共同提交／重连、五个中间阶段的析构回滚、活动名称、回收站祖先、
事务内鉴权、预留及第二作用域失败回滚、revision 与嵌套事务。共享案例源文件未修改。
配额／审计的 11 个专项案例覆盖：两种计费作用域、私有空间、双维度原子检查、整数边界、缺失／损坏额度、
审计字段和重复 request ID、审计失败时资源整体回滚、零行写入、最后节点额度竞争，以及 fixture 隔离／重置和失败播种。

专项验收的 7 个案例覆盖：WAL 读快照在其他连接提交后保持不变、读事务阻止 TRUNCATE checkpoint 及释放后的成功 checkpoint、
有界 busy 等待和迁移锁竞争、同步／日志设置降级拒绝、真实 SQLITE_LOCKED、非数据库文件识别，以及 WAL 写入／同步故障。
故障测试使用 [SQLite VFS](https://www.sqlite.org/c3ref/vfs.html) 与
[文件 I/O 接口](https://www.sqlite.org/c3ref/io_methods.html) 包装默认 VFS，除一次指定失败外仍执行真实本地 I/O。
验证提交失败不返回成功或安全重试、关闭仓储、保留扩展码，以及重开后目录／revision／两类额度／审计整体一致。
这是 VFS 边界的可控故障注入，不是磁盘硬件故障或掉电测试。

Release 验证使用 `cmake-build-release`，配置为 `-DCMAKE_BUILD_TYPE=Release -DANOF_BUILD_TESTS=ON`，构建和筛选相同的 SQLite 目标。测试工厂与 VFS 注入代码不链接入生产库。

测试在 `/tmp` 的专用目录创建真实 SQLite 数据库，顺序运行、RAII 清理。通过限制数据库页数触发 SQLITE_FULL，
不填满磁盘；大数边界只使用合成元数据。提交不确定测试用 SQLite authorizer 拒绝事务结束，
它验证错误处理，不能证明真实 I/O 故障或断电恢复行为。
共享契约通过只证明现有案例覆盖的持久化行为，
不代表完整应用用例、上传配额生命周期或全部故障场景已经验收。

## 实例启动维护

[SqliteInstanceMaintenance.h](SqliteInstanceMaintenance.h) 的 `makeSqliteInstanceMaintenance()` 返回 `Result<std::unique_ptr<InstanceMaintenance>>`，成功对象非空。工厂不访问文件；`verify(lease)` 在启动准备线程同步执行，必须传入本模块 Linux 锁工厂取得的存活 lease。它不保留 lease，不打开供业务 worker 复用的连接，也不释放实例锁。

固定检查 `.anof/instance.json`：最多 4096 字节（包括读取期间增长），UTF-8 JSON 对象恰好包含 `instance_id` 和 `format_version`。前者为小写规范非 nil UUID，后者必须为整数 1；拒绝重复／未知字段、嵌套值、BOM、尾随 JSON、错误类型和不支持的版本。示例：

```json
{"instance_id":"12345678-1234-4234-8234-123456789abc","format_version":1}
```

文件必须为当前有效 UID 所有的普通文件，单硬链接、权限 0400 或 0600。数据库及存在的 WAL／SHM 必须为同一所有者的单硬链接普通文件，权限恰好 0600。拒绝符号链接及特殊文件，不修改权限。实例目录和所有祖先的约束见[实例启动契约](../../InstanceStartupContract.md)。

数据库固定为 `.anof/metadata.sqlite3`。安全打开并预检主文件的 100 字节头，要求 SQLite 魔数和 WAL 读写标记；空文件、非 WAL 数据库及任何现存 `metadata.sqlite3-journal` 均拒绝，不自动切换日志模式或恢复回滚 journal。使用正常绝对文件名打开数据库，保持 WAL／SHM 与主文件关系；重验目录身份并核对主文件 inode，主文件校验 fd 在 SQLite 连接销毁后关闭，避免干扰进程级 POSIX 锁。持锁期间仍要求同 UID／特权主体不得替换实例树或文件。

复用 `openSqliteDatabase` 验证已有迁移历史、schema 对象、连接设置和外键，然后在 query_only 读事务中执行 `pragma_integrity_check(1)`，仅接纳唯一的 `ok` 结果。诊断在 SQL 内比较，不把数据库内容带出适配器。结果数量有界，但完整性扫描时间随数据库规模增长。所有语句、事务及连接在成功和失败返回前销毁，业务 worker 随后独立打开自己的连接。

缺文件为 NotFound，格式／安全属性／完整性损坏为 IntegrityError；不匹配的 lease 为 InvalidArgument。未来 SQLite schema 版本保留现有 InternalError，未来实例格式版本为 IntegrityError；其他后端错误保留现有分类。错误不附带格式内容。维护不创建主数据库、不迁移、不播种凭据或业务记录；正常 SQLite 打开及关闭可能管理 WAL／SHM，因此不承诺文件系统绝对只读。校验不涵盖对象内容、上传恢复、GC 或完整初始化。

[SqliteInstanceMaintenance_unittest.cc](tests/SqliteInstanceMaintenance_unittest.cc) 的 11 个案例使用真实私有实例树与小型 SQLite 数据库，覆盖严格格式与大小边界、缺失／空／非数据库、未来 schema 不迁移、schema／外键／CHECK 损坏、安全属性和伴随文件、已有 WAL 接纳、非 WAL 拒绝、lease 身份以及成功／失败资源释放。维护后可重开写事务，测试不会把局部调用等同于 prepare → InstanceServer → 信号停止集成，也不证明硬件断电持久性。

## 真实准备流程组合验证

[InstancePreparation_unittest.cc](tests/InstancePreparation_unittest.cc) 通过实际 `prepareInstance` 组合 LinuxInstanceLock、TOML 配置及 SQLite 维护适配器。`anof_sqlite_instance_preparation_unittest` 注册为模块内 integration 测试，30 秒超时，使用小型临时实例树和数据库；子进程复用 [InstanceLockProbe.cc](../../system/tests/InstanceLockProbe.cc)，exec 后独立打开锁，5 秒 alarm 限制等待。

8 个案例验证：成功结果移交真实 lease 和自有配置；锁竞争短路读取；配置错误不修改已有主库和 WAL／SHM 哨兵；缺失或不安全配置不创建数据库；格式／库缺失、损坏及未来版本保留错误分类并释放资源；公共值复验先于维护；适配边界异常释放；后端结果原样透传且不自动重试。

成功准备后仅保留 lease 的三个描述符，在单独线程构造、使用并销毁业务 MetadataStore，事务结束后再销毁准备结果。失败后核对描述符数，并通过独立子进程验证能重新取锁；锁文件 inode 和内容保持不变。观测包装器仍委托生产适配器读取真实文件，另外明确注入异常、非法公共值及 Retryable／StorageFull／CommitOutcomeUnknown，以验证编排边界；这些注入不表示真实磁盘耗尽、I/O 故障或不确定提交已发生。

此目标不创建 InstanceServer、HTTP 监听或信号桥接，不能据此宣称完整 serve 生命周期或物理机持久性已验证。
