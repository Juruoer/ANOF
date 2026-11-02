# 本地文件契约

文件适配器以持有的目录描述符访问实例，不接受任意业务路径。以内容发布为例，调用者先提交数据库意图，适配器同步上传数据、以不覆盖 rename 发布并同步目录，调用者再提交数据库结果。每一步都有独立的失败现场，只有重新核验磁盘事实才能决定后续动作。

## 安全打开与分块 I/O

Store 打开已有 uploads、objects 和 quarantine，按类型化 UploadId/BlobId/QuarantineId 派生路径；对象按 ID 前两位分片。目录身份会在使用中复核，符号链接、跨设备或被替换的目录不能继续使用。权限、UID、单链接普通文件及祖先路径要求统一见[实例启动契约](../../InstanceStartupContract.md)。对象文件只读打开。

定位读写每次最多 64 KiB，先检查长度、偏移和 off_t 范围，再处理 EINTR 和短读写。提前 EOF 是完整性错误，不能补零伪装为完整内容。增量 SHA-256 状态由调用者独占并逐块更新；传输端口没有隐式整文件读取循环。

`stat` 和 inventory 返回受检 FileEvidence，包括设备、inode、长度、mtime/ctime 纳秒。大小相同不等于文件身份相同，快速枚举不计算全文件哈希。inventory 分页返回未知普通文件的无类型化键证据，不自动删除；不安全条目返回完整性错误。capacity 使用 statvfs，并检查容量乘法溢出。

## 发布、删除与未知效果

独占创建后同步文件和父目录，截断后同步数据。发布使用 `renameat2` 的 NOREPLACE 语义，先同步数据，再同步源、目标及对象分片的父目录；不覆盖目标，也不退化为复制。删除后同步父目录，即使文件已经不存在也要完成目录同步。

错误区分 NotFound、AlreadyExists、NoSpace、Transient、Integrity 与 UnknownEffect。部分写入、rename 或 unlink 已发生后遇到错误，可能留下已改变的文件系统；UnknownEffect 不表示回滚。调用者应保留发布意图并读取新证据，不能直接重复元数据扣账。

`syncPublication(upload, blob)` 用于源已消失、目标已存在时补齐同步。调用者先依据持久意图验证完整长度和 SHA-256，并在同步及随后提交期间保持独占资格。适配器检查源缺失、目标安全和目录身份，依次同步对象数据、分片、uploads、objects，再次复核身份和源缺失。它不创建、移动、覆盖或删除文件。

源仍存在、目标缺失或条目不安全时返回 Integrity；同步不确定返回 UnknownEffect。只有重新核验证据后才能重试。补齐同步成功仍不代表数据库发布事务已经完成。

## 离线维护文件

维护方法以本次 `MaintenanceContext` 进行观察、复制、截断和隔离，使用 64 KiB 缓冲区处理内容及 SHA-256。对象分片确认缺失时 observe 可返回不存在，但仍须先确认 objects 父目录安全。

隔离前由调用者提交 PREPARED 意图；适配器执行不覆盖 rename，并同步两端目录。`verifyAction` 重新观察相关位置，核对事实并完成必要的文件和目录同步，才返回绑定动作 revision 的 Durable 收据。HELD 元数据使用 rename 后的新证据，数据库推进规则见 [SQLite 维护](../../persistence/sqlite/MAINTENANCE.md#修复意图文件收据与提交)。

Backup 和 Inventory 只保存创建时 operationId 的值，不保留上下文或取消标志引用。后续方法先校验本次 ID，再检查取消；入口拒绝不推进分页。Inventory 扫描开始后被中断会失效，调用者必须重新创建，防止丢失已消费却未返回的条目。Backup 按显式位置重读并复验索引，可以用新上下文在取消后继续读取；manifest 使用自有缓存。

`copyBackup` 在产生文件前核对 Backup 的操作 ID，向分页方法转发当前上下文；跨操作必须重新 openBackup。复制开始后的失败会标记目标失败、保留暂存产物并报告 Unknown，不能继续封存或发布。rename 成功后的迟到取消不覆盖真实成功或父目录同步未知的结果。清理资源不依赖取消标志。

## 备份与恢复发布

产品备份的 manifest 与 JSONL 索引使用有界严格解析，要求唯一递增 ID 和完整文件集合。共享 Blob 只复制一次；COMPLETE 保存 manifest 原始字节的摘要。验证同时检查对象摘要和额外文件，不能仅以完成标记存在认定备份有效。

原始保全制品使用独立的 `raw-manifest.json`，记录源 metadata/inventory 摘要和文件清单。`verifyOriginal` 核对受限 JSON、完成标记、逐文件哈希及精确平面目录集合，不打开副本 SQLite。原始保全与规范化产品备份是不同格式，`openBackup` 不接受前者。

恢复目标来自[离线目标获取](../../system/README.md#离线获取和捕获)，在同一父目录的 staging 中准备。`copyBackup` 不继承运行配置，调用者须用[统一配置序列化器](../../config/README.md)显式写入完整配置，并先完成数据库恢复准备。

封存按文件、分片、objects、.anof、staging 的子到父顺序同步；空对象恢复也必须同步 objects。必要同步失败不能将目标标为 sealed 或 publishable，发生文件变化后保留 Unknown 和 staging。发布以不覆盖 rename 完成，再同步目标父目录。失败 staging 和已发布但同步未知的目标都留给调用者复核，不自动删除或覆盖重试。
