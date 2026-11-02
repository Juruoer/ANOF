# 系统资源适配器

system 为实例提供 Linux 排他锁、初始化文件协议、离线捕获和 UTC 时钟。启动时先取得锁，配置与数据库适配器借用这把锁访问实例；停止时，全部业务资源清理完毕才销毁 lease。离线检查也使用同一个实例锁，从而与服务互斥。

## 工厂和协作

| 入口 | 用途 |
|---|---|
| [makeLinuxInstanceLockProvider](LinuxInstanceLock.h) | 普通启动时获取已有实例的锁，返回独占 InstanceLease |
| [makeLinuxInstanceInitializer](LinuxInstanceInitializer.h) | 在空根目录创建初始化文件，并持有 InitializationLease |
| [makeLinuxOfflineInstanceAccess](LinuxOfflineInstanceAccess.h) | 非阻塞获取既有锁、捕获检查副本、获取新目标 |
| [makeSystemClock](SystemClock.h) | 将系统墙上时间转换为应用使用的 UTC 微秒值 |

工厂位于 `anof::infrastructure::system`，普通工厂返回 `Result`，初始化工厂返回带阶段信息的 `InitResult`。对象由调用方独占。文件工厂的构造不获取锁或创建实例，实际操作在对应方法中同步执行，应放在启动、离线流程或适当的 worker 上。

[实例启动契约](../InstanceStartupContract.md) 是路径、权限、锁文件、初始化标记和普通启动顺序的主要参考。这里的目录/文件工具只提供安全访问，TOML 由 [config](../config/README.md) 解析，SQL 由 [SQLite](../persistence/sqlite/README.md) 处理。

## 离线获取和捕获

`acquireExisting` 只打开已有 `.anof/instance.lock`，非阻塞取锁；不要求数据库健康，不补建源文件，也不写锁内容。持锁后，`captureInspection` 将数据库、WAL/journal、配置和格式文件复制到实例外的私有临时目录，前后核对文件身份、大小、时间及目录成员。源 SQLite 不会被打开。SHM 作为原始证据保存为 `.raw` 文件，不让 SQLite 把它当作恢复依据；同时存在 WAL 和 journal 等不支持组合会得到不完整结果。

捕获的 `configurationFingerprint` 单独绑定配置和格式文件原始字节，用于修复重入时识别配置变化。目录清单通过小批次扫描保持内存上限；这可能重复遍历大目录，不能视为恒定耗时。检查 Reader 必须先于 capture 销毁，capture 再先于实例 lease 销毁。成功返回的 capture 拥有临时副本并在析构时清理；失败后残留的现场不靠再次操作源目录来修复。

`acquireDestination` 拒绝已有目标及与源目录的祖先/后代重叠，取得由目标 basename 派生的稳定旁路锁，在同一父目录创建 staging。staging 是尚未发布的工作目录；后续由文件适配器校验、同步并以不覆盖 rename 发布。目标对象析构释放资源，但保留失败 staging；旁路锁文件不 unlink。多实例/多目标的加锁顺序由上层编排，单次获取仍然非阻塞。文件发布规则见[本地存储契约](../storage/local/CONTRACTS.md#备份与恢复发布)。

## 时钟如何表示时间

`SystemClock::now()` 每次调用 `std::chrono::system_clock::now()`，通过[转换函数](internal/UtcTimeConversion.cc)返回 Unix epoch 微秒，不缓存、不改变系统时间、不使用本地时区。它是事件时间来源，可能因校时倒退；超时和耗时测量应使用单调时钟。

转换用 `std::chrono::floor` 向负无穷取整，epoch 之前也遵循同一规则：999 ns 得到 0 μs，−1 ns 得到 −1 μs，−1001 ns 得到 −2 μs。微秒是表示单位，不保证硬件测量精度。

编译期约束要求原生刻度是微秒或其整数细分、有符号整数表示且位宽不超过微秒表示。在当前 64 位纳秒表示下，最小和最大输入分别得到 −9223372036854776、9223372036854775 微秒。最小值取整后再乘回纳秒可能溢出；调用者不能无检查反向转换。`now()` 没有 Result 通道，沿用标准库读取行为，不另行检测时钟同步状态。

账户、节点等记录需要防止更新时间早于旧记录时，由仓储比较 `now`、`created_at` 和旧 `updated_at`；时钟适配器自身不做全局时间钳制。认证和到期判断继续使用原始 UTC。

## 修改时检查什么

[锁测试](tests/InstanceLock_unittest.cc)及其 [exec helper](tests/InstanceLockProbe.cc)覆盖独立进程竞争、描述符继承、路径类型、权限和锁身份。[初始化测试](tests/InstanceInitializer_unittest.cc)覆盖文件发布、重开、短写、同步/rename/unlink 故障与中断现场。[时钟测试](tests/SystemClock_unittest.cc)使用合成输入验证 epoch 两侧取整、倒退和整数边界，不修改系统时钟。

离线捕获、目标锁与维护文件组合由 [SQLite 维护专项](../persistence/sqlite/tests/SqliteMaintenance_unittest.cc)覆盖。测试注册见 [tests/CMakeLists.txt](tests/CMakeLists.txt)，运行方法统一见 [BUILDING](../../../BUILDING.md#测试)。
