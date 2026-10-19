# 上传应用用例

[UploadService](UploadService.h) 在 metadata worker 上使用同一 `TransferMetadataStore` 的短事务完成上传业务。服务只处理类型化值，不执行文件读写或等待网络／磁盘；跨阶段由调用方持有独占资格，并传入持久化 generation。

创建以用户和 clientUploadId 去重，先比较不可变原始参数，再验证目标和新预留。逻辑额度预留完整文件长度与一个节点，物理预算预留完整长度。CREATING 提交后由调用方创建并同步暂存文件，再确认 OPEN；重放不会重新预留，也不重新检查原目标是否可用。

PATCH 准备返回已授权的上传快照及结束偏移；提交要求相同 generation、OPEN、原偏移、有效 Session 与目标权限。调用者须先确认精确长度及数据同步。完成请求保存 VERIFYING，校验后进入 READY；发布意图固定 BlobId 并进入 COMMITTING。文件发布同步完成后，最终事务复验用户、取消标志和目标，原子插入 Blob／文件节点、递增父 revision、转换逻辑预留与物理计量、追加审计并保存 COMPLETED。目标冲突保留 READY 和预留，允许在同一空间改目标。

取消、过期及失败释放逻辑预留，但保留 `cleanupPending` 与物理预留。只有文件删除和目录同步确认后，`confirmCleanup` 才原子清除责任并释放物理预留；重试不重复扣账。查询发现过期时不延长寿命。上传仅对创建者可见，管理员没有读取其他用户上传的特权。storage 查询及条件预算更新仅允许管理员；磁盘采样由 runtime 补充。

[TransferRecovery](TransferRecovery.h) 提供只读 metadata 对账和每批最多100项的终态保留清理。对账核对 Blob／节点关系、P/R、空间及公共贡献额度；分页每批最多64项，目录遍历深度受128层限制。未知、缺失或不一致的数据返回错误，不归零修账。文件枚举、SHA-256、发布恢复和监听屏障属于 [UploadExecutor](../../runtime/UploadExecutor.h)。

提交返回 `CommitOutcomeUnknown` 时，调用者不能推断回滚或直接截断。runtime 使用新 Store 重读权威偏移／状态；无法确认的上传保持冻结，等待显式重新启动恢复。

验证入口为 [上传集成测试](../../../tests/integration/UploadExecutor_unittest.cc) 和 [账户服务集成测试](../../../tests/integration/AccountService_unittest.cc)，覆盖真实 SQLite／小文件流程、发布边界恢复、提交确认丢失、禁用后不可复活及过期计量。此模块不提供 HTTP 路由或 CLI 传输；文件 IO 由 runtime 分阶段调度。


[DownloadService](DownloadService.h) 的prepareDownload在单个只读快照完成身份、空间、活动父路径和文件授权，校验Node内容元数据与Blob一致，返回用户及不可变内容描述。目录返回NotAFile；不可见资源返回NotFound；不打开文件、不解释HTTP条件、不保留快照。运行时在返回下载流前完成文件类型／长度检查，并持有读取资格至磁盘清理退休。
