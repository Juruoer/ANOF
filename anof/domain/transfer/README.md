# 传输领域值

一次上传既有文件长度与偏移，也有数据库状态和空间预留。这里把它们表示成受检类型，使应用和仓储能用相同规则检查状态变化。文件读写和跨阶段调度见[传输用例](../../application/transfer/README.md)。

## 记录与状态

- [TransferValues.h](TransferValues.h)：`FileLength` 上限 1 TiB；`UploadOffset`、`UploadGeneration` 受检整数；规范十进制与小写 SHA-256 文本；范围、整数转换、磁盘安全余量计算。安全余量比例向上取整，避免低估或乘法溢出。
- [Upload.h](Upload.h)：上传记录、原始创建参数、可变发布目标、状态、固定候选 Blob、已确认偏移、清理责任和稳定失败原因。原始参数用于创建去重，不能被后续改名覆盖。
- [TransferPolicy.h](TransferPolicy.h)：记录完整性、状态迁移边界、条件替换、记录数上限与保留期。每次有效替换递增 generation；终态不复活，取消标志不清除，已确认偏移不回退。`COMMITTING → CANCELLED` 供发布恢复使用，不代表允许直接取消正在发布的文件。
- [Storage.h](Storage.h)：预算、已保存内容占用 P、上传预留 R 与独立 revision。发布 R→P，清理只扣 R；预算不变不推进 revision，零字节计量不变也不推进。超预算状态仍可清理；新预留拒绝。

## 何时释放空间

状态校验只检查给定记录，实际操作还要授权并执行事务。仓储条件写、配额、计量和审计必须共享事务。文件删除成功并同步目录后才可确认清理和释放 R，不能根据逻辑取消或 HTTP 结束推断物理预留已释放。`Blob` 保存不可变的内容描述，可变生命周期由 `BlobRecord` 保存；固定存储 key 见 [Blob.h](../drive/Blob.h)。逻辑配额预留、转用量和释放的纯计算位于 [Quota.h](../drive/Quota.h)。

## 测试

`anof_transfer_values_unittest` 使用合成整数覆盖 1 TiB、INT64 边界、摘要、状态、容量守恒、清理保留和时间边界，不创建大文件。
