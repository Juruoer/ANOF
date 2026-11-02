# SQLite 离线维护

离线维护将“看到什么”“准备改变什么”和“已经完成什么”分别保存。典型修复先持有实例锁，捕获检查副本并生成计划；随后在数据库中提交动作意图，在事务外操作文件，再以新的事务核验收据、更新计费并追加审计。检查、修复和备份的业务编排由应用服务负责，本目录提供数据库适配器。

## 检查捕获副本

[SqliteMetadataInspection](SqliteMetadataInspection.h) 只接受离线捕获副本。它允许 SQLite 在副本中恢复日志，然后进入 query-only 快照，检查结构、完整性、外键、树归属和账本。源文件捕获及寿命规则见 [system](../../system/README.md#离线获取和捕获)。

打开 Reader 前核对格式和数据库实例身份，identity 返回自有缓存，不因 getter 调用顺序重复增加 finding。对象、上传、隔离条目和维护运行按 ID 分页；坏行、错误格式或预算耗尽产生 findings/incomplete，不能当作健康结果。

`repairInvariant` 计算计划外数据的确定性摘要，只归一化计划允许变化的行和字段，保留账户、额度上限、节点身份和配置关联。上传发布的父 revision 按已完成上传数归一化，防止把计划自身的正常发布误判为外部修改。

## 修复意图、文件收据与提交

[SqliteMetadataRepair](SqliteMetadataRepair.h) 在已有 lease 下打开，不建 schema，也不自动恢复动作。Session 必须比事务活得更久。写事务可一起推进动作、调整账本和写入 local_operator 审计，失败污染与 RAII 回滚遵循[普通事务规则](CONTRACTS.md#读快照和写事务)。

动作只按 Prepared → Applied → Verified 推进。收据必须匹配实例、run、plan、计划摘要、action 及推进前 revision，并符合新读取的文件事实；不能仅凭旧收据继续推进。`verifyCompletedAction` 在只读事务中复验持久动作与当前后置条件。文件取证和同步必须在写事务外完成，文件规则见[本地维护文件协议](../../storage/local/CONTRACTS.md#离线维护文件)。

隔离从 PREPARED 转为 HELD 时，同事务保存目标的新文件证据，包括 rename 后的 ctime；原始证据仍在动作计划中。两种状态都只计费一次。物理账本等于 Blob charge 加隔离 charge；异常对象保留责任，不能因失去最后引用而自动释放。已证实的超预算状态允许读取和修复。

提交未知时关闭 Session，用新连接读取持久状态；不能重复计费或凭失败返回认定意图不存在。启动的结构检查与内容恢复分工见[实例启动契约](../../InstanceStartupContract.md)：未完成离线运行及变化的隔离证据必须阻止进入正常服务。

## 调用上下文与取消

Reader、RepairSession 和事务只保存创建时 operationId 的值，不保留 `MaintenanceContext` 或取消标志引用。每次方法都检查本次操作 ID，再检查本次取消；错误 ID 优先于取消，入口拒绝不消费分页位置，也不新增损坏 finding。SQLite progress handler 只在当前调用栈借用标志，所有退出路径都解除回调。

Reader 的扫描一旦被中断，调用者必须重新打开 Reader。事务内任一写调用或 commit 失败，包括入口拒绝，都禁止提交先前写入；析构回滚不依赖取消。提交前同步钩子在完整性复验后、COMMIT 前检查取消，并在实际提交或失败清理前移除进度回调。COMMIT 开始后的迟到取消不得覆盖真实成功或未知结果。

## 导出、规范化与恢复准备

[SqliteMetadataSnapshot](SqliteMetadataSnapshot.h) 使用显式密码编码预算，以 SQLite Backup API 导出到目标 staging，每次复制 128 页。导出失败或取消后可能已有暂存文件，调用者需保留现场并检查目标状态，不能假定文件不存在。

规范化只修改副本：删除 Session、邀请、上传、无节点引用 Blob、隔离及维护运行记录，保留用户密码、活动树、回收站和历史审计，再重算账本。UNAVAILABLE 对象、被节点引用的非 AVAILABLE 对象或未完成维护运行阻止规范化，不会通过备份掩盖异常。

验证副本时复核 schema、已引导状态、账本和应清空的记录。随后截断 checkpoint、转为 DELETE 模式、关闭数据库，确认无伴随文件并同步后计算摘要。恢复准备重新验证备份和实例身份，再切换 WAL；配置由调用者显式写入，文件适配器在数据库准备完成后才能封存和发布目标。完整发布顺序见[备份与恢复发布](../../storage/local/CONTRACTS.md#备份与恢复发布)。

## 审计与完成运行保留

[SqliteAuditRetention](SqliteAuditRetention.h) 对五类审计表共用一个总删除预算。每张表先按过期索引取得有界候选，再按表轮转删除，避免一个长期积压的表阻塞其他表。轮转位置只在确认提交后推进，失败不会声称已删除。

完成的维护运行按 90 天规则清理；仍有未验证动作、隔离、异常对象或上传责任的运行，不会仅因年龄到期被删除。保留清理不替代修复确认，也不能用来绕过启动阻断。

[维护专项测试](tests/SqliteMaintenance_unittest.cc)覆盖真实小型数据库、副本、取消借用寿命、失败事务、未知提交和文件故障。测试入口见 [README](README.md#修改时检查什么)，有限故障注入不构成物理断电耐久性证明。
