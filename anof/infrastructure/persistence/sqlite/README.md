# SQLite 元数据适配器

SQLite 保存账户、空间、节点、上传和维护记录。本目录把应用的类型化端口映射到 SQL，并保证一次业务写入中的计费与审计共用事务。内容文件由本地存储适配器处理，数据库连接不会替调用者发布或删除文件。

例如上传完成时，应用先在事务中记录发布意图并提交，再在事务外验证和发布文件，最后开启新事务插入 Blob、建立文件节点、结算预留额度并写审计。最后一次提交结果未知时，调用者必须关闭旧 Store，用新连接读取持久状态，才能决定继续哪一步。这个过程的数据库规则见[事务与仓储契约](CONTRACTS.md)，文件规则见[本地文件契约](../../storage/local/CONTRACTS.md)。

## 接入入口

[SqliteMetadataStore.h](SqliteMetadataStore.h) 提供 Metadata、Transfer、Drive 三种类型化 opener。它们使用同一个具体实现和连接，调用者选择所需端口即可，不需要向下转换。打开参数显式指定密码编码读取预算；数据库必须已完成引导。Store opener 不获取实例锁，也不代替配置、格式文件和启动恢复检查。

Store 独占连接，快照和事务借用 Store；这些对象都在所属元数据线程使用，先销毁快照或事务，再销毁 Store。一次只能有一个活动读写作用域。仓储返回自有值，不向调用者暴露 SQLite 行缓冲区或 SQL。

| 阅读目的 | 入口 |
|---|---|
| 连接设置、读快照、提交失败和仓储条件更新 | [事务与仓储契约](CONTRACTS.md) |
| 检查副本、修复证据、备份及审计保留 | [离线维护](MAINTENANCE.md) |
| 创建数据库、引导和普通启动 | [实例启动契约](../../InstanceStartupContract.md) |
| 当前表、索引、触发器及结构校验 | [current.sql](schema/current.sql)、[SqliteSchema](SqliteSchema.cc) |
| 应用如何组合这些端口 | [应用端口说明](../../../application/ports/README.md) |

## 修改时检查什么

[局部测试目录](tests/CMakeLists.txt)覆盖连接与 WAL、约束、仓储条件更新、快照、配额与审计原子性、文件初始化以及维护故障。共享契约通过真实 SQLite 后端工厂运行；[Drive 工厂](tests/SqliteTestDriveBackend.h)与[Transfer 工厂](tests/SqliteTestTransferBackend.cc)负责构造合法种子，不在生产仓储中绕过契约。

维护和传输测试保留提交未知、取消、故障 VFS 与文件证据等边界；根侧用例测试不能替代这些故障检查。构建和执行入口统一见 [BUILDING](../../../../BUILDING.md#测试)，不在这里维护通过数量或重复构建配置。
