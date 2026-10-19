# 本地内容文件适配器

[openLocalTransferFileStore](LocalTransferFileStore.h) 返回独占 TransferFileStore，打开已有实例内的 `.anof/uploads`、`objects`、`quarantine`。调用方须持有真实实例 lease，直到 Store、文件句柄及哈希状态全部销毁；工厂不创建实例，不取得锁。所有操作同步执行，应由磁盘 worker 调用，不能跨操作持有数据库事务。

文件键仅接受类型化 UploadId／BlobId；对象路径按 ID 的前两位分片。适配器使用目录描述符、no-follow、UID、权限、设备与 inode 检查，目录为 0700，文件为 0600 且必须是单链接普通文件。对象打开只读；检测到目录替换或跨文件系统布局即拒绝。

每次定位读写至多一个 64 KiB 块，先检查长度、偏移和 off_t 范围，再处理 EINTR 与短读写。提前 EOF 返回 Integrity。增量 SHA-256 使用 libsodium；调用方持有独占哈希状态并逐块更新，适配器没有整文件读取循环。

独占创建同步文件与父目录；截断后同步数据；发布使用 renameat2 NOREPLACE，同步数据及源、目标和分片父目录，不覆盖已有对象、不退化为复制。删除后同步父目录，文件已不存在时仍执行目录同步。失败可能留下部分写入或已发布／已删除的现场；UnknownEffect 不表示回滚，调用方必须重新读取证据。

inventory 返回有界页面，未知普通文件仅作为无类型化键的证据，不自动删除；不安全条目返回 Integrity。capacity 使用 statvfs 和受检乘法。文件错误区分 NotFound、AlreadyExists、NoSpace、Transient、Integrity 与 UnknownEffect。

`anof_transfer_files_unittest` 覆盖空／小型多块文件、已知 SHA-256、随机 ID 格式、分页、短 IO／EINTR、空间不足、同步／截断／发布／删除故障、不覆盖、危险条目、目录替换与描述符释放。故障注入位于内部 FileOperations；这些小文件测试不证明大文件吞吐、硬件断电持久性或恢复编排正确性。

## 已发布对象的恢复同步

`syncPublication(upload, blob)` 用于源已消失、目标已存在时补齐同步。调用方先根据持久意图核验完整长度／SHA-256，并在整个操作与后续提交期间保持独占资格。适配器检查源缺失、目标普通单链接文件和目录身份，依次同步对象数据、目标分片、uploads、objects，并再次检查身份和源缺失；不创建、搬运、覆盖或删除文件。

源仍存在、目标缺失或不安全条目返回 Integrity；同步不确定返回 UnknownEffect，可在重新核验证据后重试。测试覆盖 rename 成功后的失败、重新打开／校验、四个同步点故障和重复成功，记录实际同步的路径；不把补齐同步当作元数据发布已完成。
