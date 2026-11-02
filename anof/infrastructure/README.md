# 基础设施适配器

infrastructure 把应用端口接到 SQLite、本地文件系统和 libsodium。应用通过类型化仓储读写元数据，通过文件端口处理内容；SQL、文件描述符和第三方库细节留在这里。服务线程和离线命令的装配由 [runtime](../runtime/README.md) 负责。

以创建目录为例，元数据线程先打开 Store，再开始一个写事务。在这个事务内，应用完成授权、父目录检查、节点插入、额度计费和审计，最后一次提交。适配器保证这些数据库变化一起成功或回滚。如果操作还需要发布内容文件，调用者必须先结束数据库事务，再把文件工作交给磁盘线程，随后以新的事务确认结果。

## 从哪里开始

| 要接入或修改的行为 | 阅读入口 |
|---|---|
| 连接、快照、目录和账户仓储 | [SQLite](persistence/sqlite/README.md)；详细规则见[事务与仓储](persistence/sqlite/CONTRACTS.md) |
| 实例创建、锁、路径权限和启动检查 | [实例启动契约](InstanceStartupContract.md)；系统工厂见 [system](system/README.md) |
| TOML 读取、初始化及恢复配置 | [config](config/README.md) |
| 密码、凭据摘要、请求 ID 和随机实体 ID | [crypto](crypto/README.md) |
| 上传分块、内容发布与文件证据 | [本地存储](storage/local/README.md) |
| 离线检查、修复、备份和恢复的数据库部分 | [SQLite 离线维护](persistence/sqlite/MAINTENANCE.md)，配合[维护文件协议](storage/local/CONTRACTS.md#离线维护文件) |

## 装配和资源寿命

工厂位于 `anof::infrastructure::{sqlite,system,config,crypto,storage}`，返回类型化端口的独占对象。先检查构造结果，再将对象交给使用它的线程；成功创建一个适配器并不意味着实例已经可以监听请求。

普通启动通过应用的 `prepareInstance` 先取得实例锁、读取配置，再检查数据库。成功后移交锁和自有配置；runtime 随后创建业务连接，完成内容恢复和对账。实例锁必须覆盖 Store、文件句柄、worker 及完成回调的全部清理，不能在收到停止通知时提前释放。

Store、快照和事务在所属元数据线程构造、使用和销毁；文件操作在磁盘线程同步执行。密码计算在密码线程或离线流程中完成，计算前结束数据库快照。密码验证的接纳预算需要与 SQLite 读取密码编码的预算一致。请求 ID provider 在启动时创建并由实例复用，销毁前等待所有使用者退出。

`CommitOutcomeUnknown` 表示数据库可能已提交；文件的未知效果表示磁盘状态需要重新核验。二者都不能解释为已经回滚。调用方应先读取权威证据，再决定如何继续。具体事务和文件恢复规则分别在上表的详细文档中说明。

## 构建和查找测试

消费者链接 `anof_infrastructure`，不要复制静态库文件或硬编码本机库路径。公开工厂头使用应用端口和标准库类型；SQLite、libsodium、toml++、JSON 的链接依赖由 [CMake 目标](persistence/sqlite/CMakeLists.txt) 及各子目录传递。依赖选择和构建命令统一见根目录 [BUILDING](../../BUILDING.md)，测试入口见[测试说明](../../tests/README.md)。

模块测试按配置、密码学、系统、SQLite 和本地文件分布。SQLite 还通过工厂运行根侧的共享后端契约；同名业务的局部故障测试与共享契约关注不同边界。各子目录提供测试源码导航，不在说明中维护易过期的通过数量。小文件、故障注入和进程退出测试不能证明物理掉电耐久性或大文件吞吐。
