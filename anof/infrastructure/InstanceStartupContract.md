# 实例路径、初始化与启动契约

一个实例把元数据、上传暂存和不可变内容放在同一私有目录中。服务启动和离线维护先取得该实例的排他锁，再读取配置和数据库；锁持续到所有文件句柄、数据库连接及使用它们的任务结束。本页是这些路径和权限规则的主要参考，供维护者修改启动、初始化及文件适配器时核对。

普通启动由应用 [prepareInstance](../application/maintenance/PrepareInstance.cc) 依次执行 `acquire → load → validate → verify`。配置错误会在数据库打开之前返回；失败释放局部 lease，成功将锁和自有配置交给 runtime。此时完成的是启动前结构检查，[runtime](../runtime/DESIGN.md)还需执行内容恢复和对账，才允许监听。

## 实例布局

以下路径相对调用者提供的实例根目录，当前实例格式和数据库 schema 均为 **5**：

```text
<instance>/
  .anof/
    instance.lock
    config.toml
    instance.json
    metadata.sqlite3
    metadata.sqlite3-wal / metadata.sqlite3-shm  （存在时）
    initialization.pending                     （初始化未收尾时）
    uploads/<UploadId>.part
    objects/<BlobId 前两位>/<BlobId>
    quarantine/<QuarantineId>
```

业务打开和普通启动不创建缺失数据库、不升级或静默重置数据。格式不支持时拒绝打开，不能删除标记或手工改版本号来绕过检查。

## 路径和权限

实例根必须是非空绝对路径，不含 NUL 或 `..` 组件。`.` 和重复分隔符可以规范化；逐级使用目录描述符、`openat`、`O_DIRECTORY` 和 `O_NOFOLLOW` 检查，不调用会跟随符号链接的 canonical 解析来放行路径。

“服务账户”指进程的有效 UID。权限按下表检查，不在失败后自动 chmod：

| 对象 | 所有权和权限 |
|---|---|
| 实例根、`.anof`、内容目录及对象分片目录 | 服务账户所有，恰好 0700，无特殊权限位；内容布局要求同设备 |
| 实例外祖先目录 | root 或服务账户所有，无 setuid/setgid，不允许 group/other 可写；root 所有且带 sticky 位的共享目录是例外，例如 `/tmp` |
| 锁、主数据库、存在的 WAL/SHM、可写内容文件 | 服务账户所有的单链接普通文件，恰好 0600，无特殊位 |
| 只读配置和格式文件 | 同样要求单链接普通文件，允许 0400 或 0600 |

普通启动要求实例根和 `.anof` 已存在；初始化只接纳调用者已创建的空 0700 根目录。`uploads`、`objects`、`quarantine` 与 `.anof` 必须位于同一设备，防止把原子移动依赖变成跨文件系统复制。

[InstanceFiles](system/internal/InstanceFiles.cc) 先用 `O_PATH|O_NOFOLLOW` 检查普通文件，再以 `O_NONBLOCK|O_NOFOLLOW` 打开并比较设备号/inode，避免打开 FIFO 或设备产生阻塞、副作用。文件名必须是单个安全组件；符号链接、硬链接别名、目录和特殊文件均拒绝。描述符设置 close-on-exec，由 RAII 关闭；Linux close 遇到 EINTR 不重试，以免误关已复用的描述符。

目录描述符稳定的是已经打开的对象，并不能禁止同 UID 或特权程序替换绝对路径。整个持锁期间，服务账户及特权主体都不得重命名或替换实例树、锁和数据库。文件系统权限隔离其他账户；这里不承诺抵御绕过协议的同 UID 恶意修改，也不提供 NFS/SMB 锁行为保证。

## 锁如何取得和释放

[LinuxInstanceLock](system/LinuxInstanceLock.cc) 对固定的 `.anof/instance.lock` 使用 `flock(LOCK_EX|LOCK_NB)`。竞争立即返回 `Conflict`，不等待或循环重试。普通 acquire 在缺锁文件时可以独占创建；已有文件不截断、不替换、不 unlink。创建不修改进程 umask；若 umask 使权限不符合约定，保留现场并失败。

初始化重开和离线 `acquireExisting` 只打开已有锁，不补建源锁文件。获锁后复验固定名称仍指向持有的 inode。lease 独占根目录、`.anof` 和锁描述符，provider 可以先销毁；lease 析构关闭资源而不删除锁文件。

`flock` 是建议锁，服务与维护工具必须共同遵守。fork 会继承描述符并可能延长锁寿命，所以服务持锁期间不 fork；独立进程测试先关闭继承副本或 exec。仅收到 runtime 的 Stopped 通知不能释放锁，应等全部使用者清理完成并销毁实例资源。

## 初始化文件与数据库如何衔接

[LinuxInstanceInitializer](system/LinuxInstanceInitializer.cc) 创建 `.anof`、稳定锁和 pending，并创建、核验和同步三个内容目录。已有 `.anof`（包括其他类型）返回 `AlreadyExists`。InitializationLease 独占 InstanceLease，`lease()` 仅借用；析构不会删除半成品。

一次初始化按以下顺序完成：

1. 取得 InitializationLease，写出完整配置。新 lease 只允许成功写一次；`openPending` 重开后不能改写配置。
2. [SqliteInstanceBootstrap](persistence/sqlite/SqliteInstanceBootstrap.cc) 独占创建 schema，再用一个业务事务写入实例身份、管理员、空间/根、额度、账本和初始化审计。
3. 引导提交并验证完成后，关闭 SQLite 连接并同步数据库及目录。数据库成功不代替文件同步。
4. `publishFormat` 复验配置与内容目录，发布 `instance.json`；最后 `clearPending` 核对身份并删除 pending、同步目录。

schema 创建和业务引导是两个事务。引导失败保留文件和 pending，不自动重做。`verifyComplete` 用于核验同一初始化操作已经提交的精确初始状态，不重新哈希密码、不补记录、不重复计费；通过后仍需发布格式和清除 pending。不能用普通业务 Store 代替这个收尾验证。

### 文件表示

初始化写出 0600、单链接文件。配置最多 65536 字节，完整十三键的格式与校验见[配置说明](config/README.md#文件内容和模式)，这里不重复维护字段表。

`initialization.pending` 最多 4096 字节，精确包含五个字段。下例仅说明格式，不应手工写入运行中的实例：

```json
{"version":1,"operation_id":"12345678-1234-4234-8234-123456789ab1","instance_id":"12345678-1234-4234-8234-123456789abc","target_schema":5,"target_format":5}
```

`instance.json` 同样最多 4096 字节，只有两个字段：

```json
{"instance_id":"12345678-1234-4234-8234-123456789abc","format_version":5}
```

解析要求小写规范非 nil UUID 和整数版本，拒绝 BOM、重复/未知字段、嵌套字段值、错误类型、越界数字和尾随 JSON；允许前后 JSON 空白。初始化解析区分 `UnsupportedFormat` 与格式损坏；普通启动把不支持或不一致的格式归为 `IntegrityError`。标记只标识实例与格式，不证明数据库已经引导或内容文件完整。

文件通过同目录、带 operation ID 的临时名独占创建，完整写入并 fsync，以 `renameat2(RENAME_NOREPLACE)` 发布，再同步 `.anof` 和根目录。`publishFormat` 可复用同一操作留下的完整匹配临时文件或已有匹配格式；不覆盖不匹配或损坏文件。每次写操作复验目录、锁和 pending 身份，清除 pending 前复验配置和格式。

rename/unlink 已发生但同步无法确认时，返回带阶段的 `PublicationOutcomeUnknown`。现场可能已经改变，不能解释为回滚，也不能以删除临时文件或标记来“重试”。数据库的 `CommitOutcomeUnknown` 则需要重新查询引导证据；两种结果分别处理。

## 普通启动验证到哪里

[SqliteInstanceMaintenance](persistence/sqlite/SqliteInstanceMaintenance.cc) 首先拒绝任何类型的 pending，再核对格式、内容目录、主数据库及伴随文件。它检查 schema 定义、完整性与外键、实例 ID、唯一启用管理员和公共空间、每用户私有空间/根、额度、密码编码读取预算及账本结构。运行后的实例允许合法用户、节点和 revision 变化；历史初始化审计可以按保留策略清理，不要求数据库一直保持初始行数。

安全数据库打开要求主文件有完整 SQLite 头、WAL 读写标记，拒绝空文件、非 WAL 库及存在的 rollback journal。打开前后核对主文件、WAL/SHM 属性；主文件 fd 保留到 SQLite 连接关闭以后，避免提前 close 干扰进程级 POSIX 锁。连接使用既有绝对文件名，以保持 WAL/SHM 命名关系。返回前结束语句、事务并关闭维护连接，业务 worker 后续自行打开 Store。

这种检查可能触发 SQLite 的 WAL/SHM 管理，不能当作源文件字节不变的检查工具。需要保留损坏源原样时，使用[离线捕获](system/README.md#离线获取和捕获)，只在副本打开 SQLite。

结构维护允许可解释的待恢复上传和超预算状态。随后应用的 [TransferRecovery](../application/transfer/TransferRecovery.cc) 使用 typed 维护状态与文件证据，拒绝未完成离线维护、变化的隔离证据和不一致内容，处理可恢复上传/DELETING 对象，再核对账本。不能把 `InstanceMaintenance::verify` 成功单独解释为内容恢复完成或可以直接监听。

## 错误与实现入口

普通启动的根参数错误为 `InvalidArgument`，必需文件缺失为 `NotFound`，路径/权限/身份/格式损坏为 `IntegrityError`；内核锁竞争为 `Conflict`，SQLite 有限等待后的竞争为 `Retryable`。配置语法或值错误为 `InvalidArgument`。ENOSPC/EDQUOT 映射为 `StorageFull`，未分类系统故障为 `InternalError`；错误不携带配置、密码或 SQL 诊断内容。

初始化接口使用独立的 `InitResult` 和 `InitStage`，区分 `AlreadyExists`、`Busy`、`IncompleteInitialization`、`UnsupportedFormat` 及未知结果，不能把它们机械替换成普通启动错误码。端口与所有权定义见 [InstanceInitialization](../application/ports/InstanceInitialization.h) 和 [InstancePreparation](../application/ports/InstancePreparation.h)。

维护上述行为时，重点查阅[锁测试](system/tests/InstanceLock_unittest.cc)、[文件初始化测试](system/tests/InstanceInitializer_unittest.cc)、[数据库引导测试](persistence/sqlite/tests/SqliteInstanceBootstrap_unittest.cc)、[准备流程测试](persistence/sqlite/tests/InstancePreparation_unittest.cc)及[初始化中断/并发测试](persistence/sqlite/tests/SqliteM2Boundaries_unittest.cc)。它们分别检查文件、事务和持锁移交，不互相替代。
