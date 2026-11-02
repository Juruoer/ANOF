# 本地内容与维护文件

本目录负责实例中的上传临时文件、不可变内容对象，以及离线维护制品。应用使用类型化 ID 指定文件，适配器负责安全打开、分块 I/O、哈希、同步和不覆盖发布；数据库状态及恢复决策由应用与 SQLite 仓储管理。

一次上传先创建临时文件并逐块写入。应用记录发布意图后，磁盘 worker 核对完整长度和 SHA-256，再把文件发布到按 Blob ID 分片的对象目录，最后由元数据事务确认结果。文件与数据库不构成一个原子事务，因此发布后的同步失败必须保留现场，由调用者重新取证。

## 选择入口

| 入口 | 负责的交互 |
|---|---|
| [openLocalTransferFileStore](LocalTransferFileStore.h) | 上传创建、定位读写、截断、内容发布与删除、增量哈希、容量和文件枚举 |
| [LocalMaintenanceFileStore](LocalMaintenanceFileStore.h) | 离线观察与动作取证、隔离、原始保全、备份验证和恢复发布 |

调用者必须已经持有真实实例 lease，并保持到 Store、文件句柄、哈希状态及任务全部销毁。工厂不获取锁或创建实例。方法同步执行，应放在磁盘 worker 或离线流程中；不能跨文件操作持有数据库事务。

路径与权限以[实例启动契约](../../InstanceStartupContract.md)为准。具体 I/O、未知效果、维护收据与制品格式见[本地文件契约](CONTRACTS.md)。离线数据库配合见 [SQLite 维护](../../persistence/sqlite/MAINTENANCE.md)。

## 修改时检查什么

[TransferFiles_unittest.cc](tests/TransferFiles_unittest.cc)覆盖短 I/O、EINTR、同步故障、不覆盖发布、危险条目、目录替换、哈希和描述符寿命。[SQLite 维护专项](../../persistence/sqlite/tests/SqliteMaintenance_unittest.cc)组合文件适配器检查取消、隔离证据、备份和恢复同步。故障注入只在内部测试接缝使用，不安装全局生产回调。

构建运行方式见 [BUILDING](../../../../BUILDING.md#测试)。小文件与系统调用故障测试不证明大文件吞吐或硬件掉电耐久性。
