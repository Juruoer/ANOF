# 文件管理应用层

[DriveService](DriveService.h) 提供更新节点、单文件复制、整树入站、恢复、永久删除、单项/分页回收站和
字面搜索。它只依赖类型化端口，在metadata worker的短事务/快照内运行，不做文件操作或等待其他worker。

每个入口重新验证Session与空间权限；其他用户私有资源不可见，公共混合归属子树仅管理员可整树修改。
五个条件写命令使用`DriveCondition = std::variant<NodeCondition, TrashCondition>`保留标签原始资源种类、ID和版本。
条件在身份与资源可见性检查后校验；更新/入站还先核对整树修改权限。合法异类条件返回PreconditionFailed，
不能因UUID/版本恰好相同而通过；仓储只接收校验后的同类条件。写条件绑定源资源；修改后、提交前复验身份。真实修改与审计共用事务，无操作不推进版本/时间或增加审计。
复制创建新ID及调用者归属，共享AVAILABLE内容，重新扣逻辑额度与节点名额，不增加物理P。

入站/恢复维持逻辑和物理用量。永久删除按实际成员及原创建者扣账，设置仍保留的COMPLETED上传结果删除证据，
最后引用移除后记录Blob无引用时间；不会提前释放P或删除磁盘文件。事件时间尊重已有记录下界，认证使用原始UTC。
失败由事务析构回滚；提交未知直接返回，不自动重试。分页绑定用户、空间、查询及过滤条件，不提供跨页快照。
树操作上限1000，深度128，搜索采用端口的有界查询预算。

[CreateDirectory](CreateDirectory.h) 在目录、逻辑额度、全局节点账本及审计的同一事务内创建目录。
节点账本还由注册和上传用例显式维护；这些操作不会依赖仓储隐式扣账。
上传PATCH不依赖目标父目录仍活动，complete/发布复验目标，冲突保持预留及READY状态以便显式改目标。

[真实SQLite应用测试](../../../tests/integration/DriveService_unittest.cc) 覆盖权限、原子失败、审计回滚、提交未知、
复制计量、回收站/历史结果、UTC回拨、上传目标变化及节点预留。部分上传夹具只驱动元数据阶段，不创建内容文件；
这些用例不验证磁盘恢复。真实文件恢复由独立的[维护集成测试](../../../tests/integration/Maintenance_unittest.cc)覆盖；服务HTTP已通过DriveDispatcher接入文件管理入口。

[MaintenanceService](MaintenanceService.h) 是受信任后台调用入口，不签发或伪造用户Session。expire在事务内
重新核对条目版本与原始UTC截止，使用MaintenanceActor审计；与用户永久删除复用[PurgeTree](PurgeTree.h)
的成员、逻辑额度、上传删除证据与Blob最后引用事务。mark提交DELETING及审计；finalize在磁盘同步确认后
复验状态/版本，删除Blob并一次性释放P。两阶段之间不持有数据库事务。未知提交由runtime的新连接读取处理。

[TransferRecovery](../transfer/TransferRecovery.h) 在启动快照中计入活动和回收站节点、共享/无引用/DELETING Blob，
核对全局节点账本及完成上传的删除证据。仅内部全量校验使用完整时间域分页读取全部回收站条目，不把该枚举时间
作为自动删除依据；实际过期清理始终使用原始UTC。该遍历保持有界内存，但不是大实例恢复耗时的性能保证。
