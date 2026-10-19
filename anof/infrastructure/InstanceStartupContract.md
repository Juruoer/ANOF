# 实例启动适配契约

本文固定 infrastructure 启动适配器的接口、文件格式与安全边界，供实现和验收使用。Linux 实例锁及安全打开工具已有实现，接口和测试说明见 system/README.md；TOML 配置加载已有实现，接口和测试见 config/README.md；实例格式和 SQLite 维护已有实现，接口和测试见 persistence/sqlite/README.md。现有 SQLite、crypto 和系统时钟行为不受此契约改动。

当前格式和 schema 均为 3；事务化引导、格式发布及 M2 启动维护已实现。启动必须拒绝 pending，并核对 settings 与格式 ID、完整身份／空间／额度及初始化审计。旧开发实例只拒绝，不升级、不删除。

## 接口与内部组织

| 位置 | 工厂 | 返回端口 |
|---|---|---|
| [system/LinuxInstanceLock.h](system/LinuxInstanceLock.h) | `system::makeLinuxInstanceLockProvider()` | InstanceLockProvider |
| [config/TomlInstanceConfiguration.h](config/TomlInstanceConfiguration.h) | `config::makeTomlInstanceConfigurationLoader()` | InstanceConfigurationLoader |
| [persistence/sqlite/SqliteInstanceMaintenance.h](persistence/sqlite/SqliteInstanceMaintenance.h) | `sqlite::makeSqliteInstanceMaintenance(options)` | InstanceMaintenance |

命名空间前缀为 `anof::infrastructure`。工厂统一返回 `Result<std::unique_ptr<端口>>`，成功值非空，可报告的分配失败为 InternalError。维护工厂显式接受 SqliteMaintenanceOptions 中的密码编码读取预算与连接选项。构造不访问实例、不获取锁、不打开数据库；实际操作发生在 acquire／load／verify。

system 内部负责描述符 RAII、逐级目录打开、安全文件读取和具体 LinuxInstanceLease。lease 独占根目录、`.anof` 目录及锁描述符，公开只暴露已校验的绝对 root。config 和 sqlite 通过 infrastructure 内部访问器借用 `.anof` 描述符，不暴露 fd 到公共端口，也不另建以路径为键的全局注册表。非本适配器的 lease 返回 InvalidArgument；调用方必须保证借用期间 lease 存活，不支持并发析构。

维护的格式 JSON 解析和数据库检查由 sqlite 适配器组织，所有 SQL 位于 persistence/sqlite；通用目录工具不接触 SQLite 或 TOML。内部文件随具体实现加入，不创建空实现或虚假成功路径。

## 路径、权限与锁

接纳非空绝对实例路径，拒绝 `..` 组件，`.` 和重复分隔符可规范化；不以 canonical 跟随符号链接。逐级从根目录使用描述符相对方式打开及核验，所有路径组件必须是真实目录。实例根与 `.anof` 必须已存在，不能自动创建。

权限约定以进程有效 UID 为服务账户：

- 实例根与 `.anof`：服务账户所有，权限恰好 0700，拒绝特殊权限位。
- 实例外祖先：所有者为 root 或服务账户，拒绝 setuid／setgid 和 group／other 可写；唯一共享可写例外是 root 所有且有 sticky 位的目录，以支持 `/tmp` 中的测试专属实例。祖先无需全部 0700。
- instance.lock、metadata.sqlite3 及存在的 `metadata.sqlite3-wal`／`metadata.sqlite3-shm`：服务账户所有，普通文件，0600，无特殊位、无硬链接别名（nlink 必须为 1）。
- config.toml 和 instance.json：同样要求普通文件、服务账户所有、nlink=1，权限允许 0400 或 0600。
- 拒绝符号链接、FIFO、设备、socket 等不合适类型。打开方式不得因 FIFO 等类型阻塞；不通过 chmod 修补外部数据。

路径检查与打开须优先保持在目录 fd 上，打开后 fstat 核对类型／权限／身份。必须明确：目录描述符只能稳定已打开对象，不能使绝对路径免受替换。持锁期间服务账户和特权主体均不得重命名或替换实例树、锁文件及数据库文件；其他主体由私有目录权限隔离。已有 runtime 的 worker 仍使用 root 下的数据库路径，此所有权假设同样适用。

锁机制固定为 Linux `flock(LOCK_EX | LOCK_NB)`，服务和离线维护共用 `.anof/instance.lock`。锁文件不存在时，仅可在已验证的 `.anof` 内以 0600 独占创建；存在时打开同一文件，不截断、不 unlink、不替换。umask 只能收紧创建权限，创建后仍按约定检查，不临时修改进程全局 umask。

所有持有描述符设 close-on-exec；acquire 失败自动关闭，成功后由 lease 独占。析构关闭描述符释放本次持有的内核锁，不显式删除文件。fork 继承的描述符可能延长锁生命周期；生产启动不在持锁期间 fork，测试独立获取必须关闭继承副本或通过 exec 隔离。Stopped 不释放 lease，最终对象析构才释放；适配器不观察 runtime 状态。

## 配置文件

固定读取 `.anof/config.toml`，包括读取过程中增长在内最多 65536 字节，超限拒绝，不回显文件内容。仅接纳以下顶层键，不接纳表、数组或其他键：

```toml
listen_address = "127.0.0.1"
port = 8080
development_http = true
metadata_queue = 256
http_in_flight = 128
shutdown_grace_seconds = 30
disk_safety_min_bytes = 1073741824
disk_safety_percent = 5
```

| TOML 键 | 类型及范围 | 对应公共字段 |
|---|---|---|
| listen_address | 字符串，仅 127.0.0.1 | listenAddress |
| port | 整数，0～65535 | port |
| development_http | 布尔，必须显式 true | developmentHttp |
| metadata_queue | 整数，1～256 | metadataQueue |
| http_in_flight | 整数，1～128 | httpInFlight |
| disk_safety_min_bytes | 整数，0～INT64_MAX，缺省 1073741824 | diskSafety.minimum |
| disk_safety_percent | 整数，0～100，缺省 5 | diskSafety.percent |
| shutdown_grace_seconds | 整数或浮点数，有限 0～30 | shutdownGraceSeconds |

以 `InstanceConfiguration{}` 为唯一默认来源，缺省键不另设默认值。developmentHttp 的默认 false 故意不通过校验。整数先检查符号和范围再缩窄；宽限期接受整数和有限浮点数，拒绝 NaN／infinity。拒绝重复定义、未知键、错误类型、溢出及 TOML 语法错误。调用现有 validateInstanceConfiguration 复验公共约束，返回自有值；load 不保留 lease 或 TOML 节点。

## 最小格式元数据

固定读取 `.anof/instance.json`，包括增长在内最多 4096 字节，UTF-8 JSON，顶层必须是对象且恰好含以下两项：

```json
{"instance_id":"12345678-1234-4234-8234-123456789abc","format_version":3}
```

instance_id 必须是小写规范分组、非 nil UUID 字符串，不强制 UUID 版本；不把它当 UserId 或节点 ID，公共端口不增加字段。format_version 必须是 JSON 整数 3，布尔、浮点数及字符串均不接受。拒绝未知／重复字段、超范围数字、额外尾随 JSON、BOM 和格式错误；前后 JSON 空白允许。使用有界解析并拒绝字段值中的嵌套结构。

此文件是独立实例格式标记，不代替 SQLite schema 版本；不隐含校验对象目录、上传恢复、GC 状态或完整 init 工作流。初始化文件由 LinuxInstanceInitializer 创建；数据库引导由 SqliteInstanceBootstrap 完成，均使用调用方提供的 typed ID。

## 维护与错误边界

调用顺序使用现有 prepareInstance：acquire → load → 公共配置校验 → verify。verify 先拒绝任何类型的 initialization.pending，再检查 instance.json、metadata.sqlite3 和存在的 WAL／SHM 的安全属性，验证已有 schema／建库定义／设置及数据库结构完整性与外键，并检查 settings 的实例 ID、唯一管理员／公共空间、用户私有空间／根、必需额度和初始化审计；密码编码必须符合显式读取预算。仅复用不带 CREATE 的数据库打开路径；禁止用 createSqliteDatabase 代替验证。完整性 SQL 位于 sqlite 内，读取结果有界；数据库扫描耗时与实际数据量相关，不能把结果数量限制说成恒定耗时保证。

SQLite 文件打开及伴随文件处理必须结合经过验证的目录身份实现，不假定 SQLite 可以直接接管一个已打开的普通文件 fd，也不能通过替换随机临时文件名破坏 WAL 身份。维护使用原有绝对文件名打开 SQLite，保留其 WAL／SHM 命名关系。打开前重新校验绝对目录与 lease 所持目录身份，打开前后核对主文件身份；验证用主文件 fd 持有到维护连接关闭后，避免提前 close 释放进程级 POSIX 锁。安全检查和已有树不可替换约定同时成立；不宣称抵御具有同一 UID／特权的恶意并发修改。

打开 SQLite 前要求主文件至少具有完整 100 字节头、SQLite 魔数及 WAL 读写格式标记（均为 2）；空文件和非 WAL 数据库返回 IntegrityError，不隐式初始化或转换日志模式。存在 metadata.sqlite3-journal 时保守返回 IntegrityError，不触发回滚 journal 恢复或自动删除。WAL／SHM 在打开前、打开后及正常关闭后检查安全属性。

verify 返回前销毁全部维护连接、语句及事务；业务连接由 worker 后续自行打开。验证不插入业务记录、不更新应用 schema、不创建缺失主数据库；SQLite 正常打开可能涉及 WAL／SHM 管理，不能承诺验证过程是文件系统层面绝对只读。非法配置必须在进入任何数据库维护前失败。

| 情况 | 分类 |
|---|---|
| 根参数不是绝对路径、含 `..`、不匹配的 lease | InvalidArgument |
| 缺失实例根、`.anof` 或必需文件 | NotFound |
| 内核锁被占用 | Conflict |
| 配置语法、键、值或 64 KiB 上限不合法 | InvalidArgument |
| 路径类型、所有权、权限、硬链接不符合约定 | IntegrityError |
| 格式 JSON 损坏／超限／不支持版本、数据库完整性损坏 | IntegrityError |
| 无权访问（EACCES／EPERM）、未分类系统调用或分配失败 | InternalError |
| 创建锁文件时 ENOSPC／EDQUOT | StorageFull |
| SQLite 打开／验证错误 | 保留后端现有分类，包括 Retryable、StorageFull 等 |

当前 schema 3 校验对不支持的版本返回 IntegrityError；格式标记的未来版本为 IntegrityError。错误不回显配置／格式内容，也不把无法确认的提交结果转换为可安全重试。锁和普通文件系统操作不额外引入业务重试循环。

## 构建与验证边界

启动与初始化适配器使用同一 anof_infrastructure 目标；TOML 依赖 `tomlplusplus::tomlplusplus`，JSON 依赖 `nlohmann_json::nlohmann_json`，均私有链接。公开头只依赖现有 application 端口和标准库。

锁和安全文件打开已接入局部真实进程／文件测试。TOML 配置的有界读取和解析已有局部测试；instance.json 格式校验、维护关闭、真实 schema／外键／完整性检查已有局部测试。InstancePreparation_unittest.cc 通过实际 prepareInstance 组合三个生产适配器，覆盖成功移交、配置短路、维护失败、异常释放、跨进程再取锁及另一线程独立打开业务仓储；边界注入的错误只验证分类透传，不冒充真实 OS 故障。完整 serve／信号停止组合保持在根集成范围。

内容目录 uploads、objects、quarantine 由初始化创建并同步；启动维护在打开 SQLite 前检查其所有权、0700 权限、身份及同设备属性。物理预算单例必须存在，允许合法待恢复上传和超预算读／清理状态；文件归属、计量对账与恢复编排不在结构维护入口内执行。
