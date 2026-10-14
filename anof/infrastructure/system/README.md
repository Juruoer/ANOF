# 系统适配器

[SystemClock.h](SystemClock.h) 提供 `anof::infrastructure::system::makeSystemClock()`，返回 `Result<std::unique_ptr<Clock>>`。实现通过现有 `Clock` 端口提供 Unix epoch 微秒时间，链接目标为 `anof_infrastructure`。

## 构造与使用

装配方在元数据 worker 内调用工厂，检查成功后把独占对象放入 worker 资源集合，并在该线程使用和销毁。工厂无参数，不依赖 runtime、HTTP、数据库或 libsodium 初始化。对象分配失败返回 `InternalError`；成功结果持有有效对象，不返回空指针。

[SystemClock.cc](SystemClock.cc) 每次 `now()` 读取 `std::chrono::system_clock::now()`，通过[纯转换函数](internal/UtcTimeConversion.cc) 返回 `UtcTime`。接口不调整系统时间，不缓存时间，不进行本地时区转换或 HTTP 文本格式化。生产构造不接受时钟替身。

## 时间语义与范围

`UtcTime` 是以微秒为 duration 的 system_clock 时间点。在当前 Linux 环境中，system_clock 以 Unix epoch 为起点，原生刻度为有符号 64 位纳秒。子微秒部分通过 C++17 `std::chrono::floor` 向负无穷取整：

| 原生 epoch 偏移 | 返回的 epoch 微秒 |
|---|---:|
| 0 ns | 0 |
| 999 ns | 0 |
| 1000 ns | 1 |
| −1 ns | −1 |
| −1000 ns | −1 |
| −1001 ns | −2 |

转换不经过 time_t、浮点数或本地时区。实现通过编译期约束要求原生表示为有符号整数、刻度为微秒的整分单位、数值位宽不超过微秒表示，以保证转换只缩小刻度且不会溢出。不兼容的平台需显式适配，不能静默采用危险转换。

当前原生纳秒最小／最大值分别转换为 −9223372036854776 和 9223372036854775 微秒。最小值向下取整后的微秒若再乘回纳秒会超出原生范围；调用方不应无检查地反向转换。UtcTime 自身可表示的范围比本机原生时钟宽，不代表所有 UtcTime 都能转换为原生纳秒。

微秒是返回值的表示精度，不保证系统时钟的测量精度或同步准确度。墙上时间可因系统校时而回退，不适合测量单调时长或实现超时。`Clock::now()` 没有 Result 通道，适配器沿用标准库系统时钟读取行为，不另行检测系统校时、同步状态或报告底层时钟读取错误。

## 构建与测试

[CMakeLists.txt](CMakeLists.txt) 把实现加入 `anof_infrastructure`，无需新增系统库依赖。开启 `ANOF_BUILD_TESTS` 时提供：

- `anof_system_headers_check`：SystemClock.h、LinuxInstanceLock.h 与 LinuxInstanceInitializer.h 独立编译，仅依赖 `anof_application`。
- `anof_system_clock_unittest`：[4 个 Boost.Test 案例](tests/SystemClock_unittest.cc)，CTest 超时 15 秒。覆盖类型与生产工厂调用、epoch 前后的取整、2038／2106 数值及负时间、倒退输入、原生最小／最大值和邻近整微秒边界。

从项目根目录运行：

```sh
cmake --build cmake-build-debug --target anof_system_clock_unittest anof_system_headers_check --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_system_clock_unittest$' -j 1
```

Release 使用 `cmake-build-release` 和相同目标，两者均需开启 `ANOF_BUILD_TESTS`。边界测试明确针对当前 Linux 64 位纳秒表示，使用受控转换输入，不改变系统时钟、不睡眠、不要求两次墙上时间读数递增。没有注入真实内存耗尽或系统时钟故障，不据此声称 Session 到期或完整 runtime 集成已验收。

## Linux 实例锁

[LinuxInstanceLock.h](LinuxInstanceLock.h) 提供 `makeLinuxInstanceLockProvider()`，返回 `Result<std::unique_ptr<InstanceLockProvider>>`。工厂无文件操作，分配失败返回 InternalError。`acquire(root)` 返回独占 lease；provider 可先于 lease 销毁，锁仍保持。返回 root 是通过逐级检查获得的绝对路径，不跟随符号链接。

### 路径及权限

root 必须为非空绝对路径，不含 `..` 或 NUL；允许 `.` 和重复分隔符。所有路径组件逐级使用 `openat`、`O_DIRECTORY`、`O_NOFOLLOW` 检查，实例根和 `.anof` 必须已存在，服务账户有效 UID 所有，权限恰好 0700，无特殊权限位。实例外祖先必须由 root 或服务账户所有，拒绝 setuid／setgid 及 group／other 可写；root 所有且有 sticky 位的共享目录为可写例外，允许 `/tmp` 中的私有测试实例。

锁文件固定为 `.anof/instance.lock`，必须为服务账户所有的 0600 普通文件，硬链接计数为 1。不存在时使用 O_CREAT|O_EXCL 创建，不修改进程 umask；创建权限被 umask 收紧而不满足约定时失败，保留该文件供运维检查，不自动 chmod 或删除。存在时不截断、不替换文件。

[内部目录和文件工具](internal/InstanceFiles.h) 使用 OwnedFd 管理描述符。普通文件先通过 O_PATH|O_NOFOLLOW 获取对象并 fstat 校验，避免 FIFO／设备的 open 副作用，再以 O_NONBLOCK|O_NOFOLLOW 实际打开，核验属性及设备号／inode 一致。只允许单个文件名组件。只读文件允许 0400 或 0600；读写文件仅允许 0600，均拒绝符号链接及非普通文件。此工具只负责安全打开；[BoundedFileRead](internal/BoundedFileRead.h) 提供最多 64 KiB 的有界读取，TOML 解析在 config 模块。

### 锁及生命周期

使用 `flock(LOCK_EX | LOCK_NB)`，竞争立即返回 Conflict，不等待或循环重试。锁文件创建发生竞争时只重开已有文件一次。获锁后核对固定名字仍对应持有的 inode。描述符均设置 O_CLOEXEC；失败路径和 lease 析构关闭描述符，不 unlink、不截断、不以 PID 文件代替内核锁。Linux close 不因 EINTR 重试，避免误关已复用描述符。

flock 是建议锁，所有服务／离线维护入口必须遵守同一锁协议，不能阻止有权限的外部程序直接修改文件。fork 继承的描述符共享锁，可能延长持锁时间；服务持锁期间不 fork。实现不显式 LOCK_UN，防止子进程关闭继承 lease 时意外释放父进程的锁；独立进程测试在获取前关闭继承副本或 exec。语义依据 [Linux flock 文档](https://man7.org/linux/man-pages/man2/flock.2.html) 和 [open 文档](https://man7.org/linux/man-pages/man2/open.2.html)。

lease 保留根目录、`.anof` 目录及锁描述符，供内部工具稳定访问已验证目录。`instanceDirectory(lease)` 只接受本适配器的 lease，借用期间调用方必须保持其存活。目录 fd 不防止绝对路径被替换：服务账户及特权主体必须保证整个持锁期间实例树和锁文件不被重命名／替换，不声称抵御同 UID 恶意持续修改。锁须覆盖其他实例资源的全部清理；达到 runtime Stopped 并不会自行释放它。当前支持及验证范围为 Linux 本地文件系统，不宣称 NFS／SMB 锁语义已验收。

### 错误与验证

无效根参数或不匹配 lease 为 InvalidArgument，缺失路径为 NotFound，类型／权限／所有权／硬链接／身份不符合为 IntegrityError，锁竞争为 Conflict，ENOSPC／EDQUOT 为 StorageFull；EACCES／EPERM 及其他未分类系统错误为 InternalError。错误不包含文件内容。直接由内核拒绝打开时按 errno 分类，不能保证所有权限拒绝都已进入 fstat 校验。

`anof_instance_lock_unittest` 的 [11 个案例](tests/InstanceLock_unittest.cc) 覆盖独立 fork／exec 竞争与重新获取、三个 lease fd 的 exec 关闭、锁文件 inode／内容保持、创建权限、路径规范化和拒绝、目录权限、锁文件 symlink／FIFO／目录／硬链接、特殊权限位、反复失败的 fd 数量，以及子进程严格 umask 下的失败清理。使用[独立 exec helper](tests/InstanceLockProbe.cc)，子进程设 5 秒 alarm，CTest 超时 30 秒；测试要求非 root，只修改自身 `/tmp/anof-instance-lock-*` 目录并清理。

测试未通过提权制造他人所有权，未运行并发恶意替换、网络文件系统或真实磁盘耗尽；errno 分类测试不冒充真实 OS 故障。锁测试本身不验证配置解析、实例维护或完整 serve 关闭。真实 prepareInstance 组合的持锁移交与失败释放见 [SQLite 准备流程测试](../persistence/sqlite/tests/InstancePreparation_unittest.cc)。

```sh
cmake --build cmake-build-debug --target anof_instance_lock_unittest anof_system_clock_unittest anof_system_headers_check --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_(instance_lock|system_clock)_unittest$' -j 1
```

## 初始化文件与 lease

[LinuxInstanceInitializer.h](LinuxInstanceInitializer.h) 提供 `makeLinuxInstanceInitializer()`，返回 `InitResult<std::unique_ptr<InstanceInitializer>>`。构造不访问文件系统；`create(root, identity)` 要求既有、当前 UID 所有的空 0700 根，原子创建 `.anof` 并持有与普通运行相同的稳定内核锁。InitializationLease 独占 InstanceLease，`lease()` 仅供借用；工厂对象可先销毁，lease 析构只关闭资源，不清理半成品。文件操作同步执行，不属于 HTTP I/O loop。

创建成功意味着锁文件及 pending 已同步。已有 `.anof`（包括其他类型）返回 AlreadyExists，锁竞争返回 Busy。不会创建任意祖先、修改 umask、补改权限或覆盖已有文件。`openPending(root)` 只打开已有锁，不补建缺失锁文件，要求可信 pending 和完整配置；缺失必要文件返回 IncompleteInitialization。每次写操作复验根、实例目录、锁文件及 pending 的身份。

| 文件 | 当前表示与上限 |
|---|---|
| initialization.pending | 最多 4096 字节；精确五字段：version=1、operation_id、instance_id、target_schema=2、target_format=2 |
| config.toml | 最多 65536 字节；完整六个配置键，见[配置参考](../config/README.md) |
| instance.json | 最多 4096 字节；仅 instance_id、format_version=2 |

JSON 严格拒绝 BOM、重复／未知字段、嵌套值、非整数版本及尾随 JSON；ID 为小写规范非 nil UUID。格式不合法为 IntegrityError，不支持的整数版本为 UnsupportedFormat。新文件均为 0600、单链接普通文件，描述符带 CLOEXEC；只读检查沿用既有工具允许 0400 或 0600。

`writeConfiguration` 校验并写出完整 TOML，只能在新 lease 上成功一次；重新打开的 lease 不允许改写配置。文件发布使用包含 operation ID 的同目录临时名，独占创建、完整写入、fsync、renameat2(RENAME_NOREPLACE)，再同步 `.anof` 和根。失败保留现场，不自动重试、截断或清理临时文件。格式发布可复用同一操作留下的完整、匹配且安全的格式临时文件；损坏或不匹配的临时文件保留并拒绝。

`publishFormat()` 要求配置仍与 lease 保留的值一致；已有匹配格式只复验和同步，不重写内容。成功后才允许 `clearPending()`；后者核对 pending 的 ID 和 inode、配置及格式，删除 pending 并同步目录。发布后同步失败，以及无法确认 rename／unlink 结果的错误，返回 PublicationOutcomeUnknown，不表示回滚；其他错误保留对应 stage。错误结果不包含文件内容。

这些接口只实现文件协议，不打开数据库，也不证明引导完成。装配方必须在同一 lease 下完成数据库引导或 `InstanceBootstrap::verifyComplete`，关闭维护连接后才能发布格式并清除 pending。[SQLite Bootstrap 与启动维护](../persistence/sqlite/README.md) 已实现上述数据库检查；文件模块测试本身不代表根 CLI 的 `init`、`--finish` 或 serve 已接通。

`anof_instance_initializer_unittest` 的 [19 个案例](tests/InstanceInitializer_unittest.cc) 使用小型私有临时目录，覆盖重复创建、文件权限／路径／大小、严格 JSON、配置不覆盖、标记和锁替换、跨进程锁、短写与 EINTR、写入／同步／rename／unlink 故障及格式发布前后进程终止。内部 syscall 替身只用于故障测试；进程退出与故障注入不证明硬件断电持久性。CTest 超时 30 秒，子进程 alarm 为 5 秒。

```sh
cmake --build cmake-build-debug --target anof_instance_initializer_unittest anof_instance_lock_unittest anof_instance_configuration_unittest anof_system_headers_check anof_config_headers_check --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_(instance_initializer|instance_lock|instance_configuration)_unittest$' -j 1
```

Release 使用现有 `cmake-build-release` 及相同目标。测试中的文件发布独立于数据库，仅验证上述文件边界。
