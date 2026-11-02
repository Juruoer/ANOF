# 离线维护领域值

检查工具需要描述发现的问题，修复工具需要记录准备修改什么、已经完成到哪一步。这个目录提供这些共同的数据类型，供应用与具体适配器交换维护结果。

[MaintenanceValues.h](MaintenanceValues.h) 定义独立运行/计划/动作/隔离/备份 ID、类型化 finding、报告、 修复计划、持久动作证据和备份清单，不包含 SQL、JSON 对象、文件描述符或文件系统操作。

## 修复计划与证据

计划只允许七类既定动作；结构校验拒绝版本不符、重复动作 ID、重复隔离目标、超限、未完成检查和缺少 full 检查的内容恢复确认。该校验不授予执行权限：执行入口仍必须重新取证、重建合法动作并核对计划。 动作 APPLIED/VERIFIED 要求完整 Durable 证据及正确的前后 revision；证据绑定和动作文件事实校验不证明实际磁盘效果，文件适配仍需真实取证与同步。隔离计费必须等于记录的文件长度。检查未完成或报告截断时，报告状态始终 incomplete，不能以没有详细 finding 推断健康。

## 备份清单

文件键只由 BlobId、UploadId、QuarantineId 推导。备份索引验证按 BlobId 严格递增、禁止重复，常量内存累加项数/总字节；清单限制版本、数据库后端、配置大小和索引规模。编码、文件集合、摘要、原子发布及持久性不由这些值校验完成。

## 测试

[MaintenanceValues_unittest.cc](tests/MaintenanceValues_unittest.cc) 使用内存值测试报告、计划、异常内容证据、 隔离、备份索引、超预算释放、统计子集、Origin 和配置边界，实际检查和恢复流程见[维护应用](../../application/maintenance/README.md)。
