# 密码、凭据和标识适配器

crypto 使用 libsodium 实现应用需要的密码哈希、随机凭据、摘要和 ID。它不查询用户、不写 Session，也不决定登录成功；这些决定由应用根据数据库中的账户与会话状态作出。

一次登录先由元数据线程读取密码快照并结束数据库作用域，再由密码线程调用 `PasswordHasher::verify`。验证成功后，应用复核账户版本，生成随机凭据，将摘要与 Session 一起提交，最后才交付明文 token。后续请求只需要把 token 解码成摘要，再交给授权仓储；不会重新运行密码哈希。这样的拆分既避免占用 I/O loop，也避免在昂贵计算期间持有数据库事务。

## 选择入口

工厂位于 `anof::infrastructure::crypto`，返回 `Result<std::unique_ptr<端口>>`，消费者链接 `anof_infrastructure`。

| 需要的能力 | 入口 |
|---|---|
| 密码哈希与验证 | [makeSodiumPasswordHasher](SodiumPasswordHasher.h)，显式传入 [SodiumPasswordPolicy](SodiumPasswordPolicy.h) |
| Session 或邀请的随机明文和摘要 | [makeSodiumCredentialIssuer](SodiumCredentialIssuer.h) |
| Session token 解码与摘要 | [makeSodiumSessionTokenCodec](SodiumSessionTokenCodec.h) |
| 邀请 token 解码与摘要 | [makeSodiumInviteTokenCodec](SodiumInviteTokenCodec.h) |
| 同一服务实例内的请求关联 ID | [makeSodiumRequestIdentity](SodiumRequestIdentity.h) |
| 节点、账户和实例等随机 ID | [NodeIdGenerator](SodiumNodeIdGenerator.h)、[IdentityIdGenerator](SodiumIdentityIdGenerator.h) |
| Upload/Blob、Trash 及维护实体随机 ID | [Transfer/Drive/Maintenance 工厂](SodiumTransferIdGenerator.h) |
| 维护计划编码的 SHA-256 | [SodiumMaintenanceDigest](SodiumMaintenanceDigest.h) |

密码策略区分“生成新密码哈希的参数”和“接受既有编码的上限”。它必须与 SQLite 的密码编码读取预算一致，不能把适配器硬上限当作生产工作参数。参数范围、token 编码、请求 ID 派生和秘密清理见[密码学适配契约](CONTRACTS.md)。

## 构造和调用寿命

工厂在启动阶段完成 libsodium 初始化，可能等待系统随机源；不要在同步请求 selector 中反复构造。Session codec 不含请求状态，摘要方法只处理固定大小输入。RequestIdentity 为每个实例创建一次并复用，销毁前等待使用者退出。

密码和随机签发工作在 worker 或离线流程执行。适配器不创建线程、队列或内存调度器；这些资源策略由 [runtime](../../runtime/README.md) 管理。摘要成功只说明输入编码合法，不表示 Session 未到期或未撤销。请求 ID 用于关联日志，不能用于授权或作为业务幂等键。

## 修改时检查什么

[密码测试](tests/PasswordHasher_unittest.cc)覆盖真实算法往返、预算预检、错误密码与计算失败的区分；[凭据测试](tests/M2Credentials_unittest.cc)和 [Session codec 测试](tests/SessionTokenCodec_unittest.cc)覆盖已知摘要、规范编码及秘密移动。[请求 ID 测试](tests/RequestIdentity_unittest.cc)核对精确输入布局与失败路径；[节点 ID](tests/NodeIdGenerator_unittest.cc)和[维护 ID 测试](tests/MaintenanceId_unittest.cc)检查类型和 UUID 位布局。

测试中的固定输入只进入内部接缝，不给生产工厂增加固定种子或替代随机源。有限样本检查不能证明绝对无碰撞。测试注册见 [tests/CMakeLists.txt](tests/CMakeLists.txt)，构建运行方式见 [BUILDING](../../../BUILDING.md#测试)。
