# 运行时装配与调度

runtime 把应用用例、具体适配器和 HTTP 入口装配为可启动、可停止的服务。数据库、密码和文件工作在有界 worker 中执行，完成结果返回所属 EventLoop；HTTP 线程不等待这些阻塞工作。

例如创建目录时，HTTP 层提交一个命令，runtime 将它交给元数据线程执行，收到结果后再回到事件循环发送响应。上传和登录需要多个阶段，runtime 还负责在这些阶段之间保留任务状态，并在请求取消后完成必要清理。

## 主要组件

| 组件 | 职责 |
| --- | --- |
| [InstanceServer](InstanceServer.h) | 实例准备后的服务装配、监听与停止协调 |
| [MetadataExecutor](MetadataExecutor.h)、[IdentityExecutor](IdentityExecutor.h)、[DriveExecutor](DriveExecutor.h) | 元数据、身份和文件管理调度 |
| [DiskExecutor](DiskExecutor.h)、[UploadExecutor](UploadExecutor.h) | 上传、下载、文件工作及取消后的清理 |
| [MaintenanceExecutor](MaintenanceExecutor.h)、[BlobAccessRegistry](BlobAccessRegistry.h) | 有界过期清理与内容读删互斥 |
| [InstanceMaintenance](InstanceMaintenance.h)、[OfflineRecovery](OfflineRecovery.h) | 离线维护装配与单项恢复 |
| [LogSink](LogSink.h)、[AuditRetentionExecutor](AuditRetentionExecutor.h) | 有界诊断输出与审计保留 |

应用通过[类型化端口](../application/ports/README.md)表达任务，transport 负责 HTTP 映射。具体资源由实例装配创建；组件不自行决定用户权限或重试未知提交。

## 生命周期

启动先持有实例锁、验证配置与数据、完成必要恢复，再开放监听。停止先关闭准入，等待 HTTP 清理、任务完成回调与 worker 退出，最后释放实例锁。取消请求不等于磁盘或事务已经结束，相关资源必须保留到实际工作结束。

扩展时需要遵守的队列容量、回调所有权、恢复顺序和停止屏障见 [运行时设计](DESIGN.md)。运行服务与运维命令见 [CLI](../../apps/anof/README.md)。

## 验证

[集成测试](../../tests/integration/CMakeLists.txt)覆盖调度、队列饱和、取消、未知提交、真实 HTTP、恢复和进程停止；执行入口见 [测试说明](../../tests/README.md)。各类测试的具体范围在设计文档中列出。
