# 身份与账户服务

这里处理登录、注册、会话、账户和额度。服务运行在元数据线程，通过仓储读取或修改状态；密码计算交给独立的密码线程。[IdentityService](IdentityService.h) 处理身份与读取，[AccountService](AccountService.h) 处理账户管理，两者均不依赖 HTTP 或 SQL。

## 一次登录如何完成

`prepareLogin` 读取并返回自己持有的密码快照，然后关闭读事务。调用者验证密码后，再调用 `completeLogin`。后者在新事务中复核账户是否启用、密码版本、epoch 和用户 revision，防止计算密码期间发生的修改被忽略。

复核成功后，服务检查会话数量，写入 Session 和审计，再返回令牌。Bearer 与 Cookie 合计最多 32 个活跃会话，有效期为七天。只有确认提交成功才返回新令牌；提交结果未知时不会返回秘密或自动重试。

两种会话都要求摘要与凭据模式匹配。Web 开关、Origin 和 CSRF 仍由 HTTP 入口检查。`cleanupSessions` 每次最多删除 200 条失效会话，调用频率由 runtime 控制。

## 注册与读取

注册接受已经计算好的密码哈希，在同一事务中消费邀请、检查人数、创建用户及其私有空间和根目录、设置额度并记录审计。私有根计入一个节点。任何一步失败都会回滚；注册完成后还需要登录。

读取操作在同一快照中检查会话、空间和祖先可见性，再核对分页游标的绑定。管理员也不能借此查看其他人的私有节点。每次只读一页，连续翻页不共享快照。

`GetMeCommand` 返回用户和实例 ID，二者来自同一次鉴权快照；实例设置缺失时返回 `IntegrityError`。邀请明文只在创建且确认提交成功后返回。登出仅在确实撤销会话时记录审计，已经失效的凭据仍会被拒绝。

## 修改账户和密码

账户管理支持本人显示名、管理员用户查询与启禁、空间及公共贡献额度。写命令携带资源种类、ID 和 revision；先鉴权与检查目标，再比较条件。条件过期时，即使新值与旧值相同也返回 `PreconditionFailed`。没有实际变化时，不增加版本、时间或审计记录。

禁用账户会递增 epoch 并撤销其全部会话，再启用不会恢复旧凭据；唯一管理员不能被禁用。额度不能降到已用量与预留量之下。账户管理不提供替他人修改登录名、角色或密码的接口。

改密也分成读取、密码计算和最终提交三个阶段。`preparePasswordChange` 返回当前密码快照；调用者验证旧密码并计算新哈希后，`completePasswordChange` 重新检查会话、条件和密码版本，再更新密码并撤销所有会话。旧密码错误也需要经过最终鉴权：会话失效返回 `Unauthenticated`，资料版本冲突返回 `PreconditionFailed`，身份有效但旧密码错误返回 `Forbidden`。成功后不签发新令牌，需要重新登录。

## 接入与测试

命令与返回值见 [IdentityCommands](IdentityCommands.h) 和 [AccountCommands](AccountCommands.h)。HTTP 调用者通过 [IdentityDispatcher](../ports/IdentityDispatcher.h) 提交任务；排队、限流和完成回调由 [IdentityExecutor](../../runtime/IdentityExecutor.h) 实现。

[身份服务测试](../../../tests/integration/IdentityService_unittest.cc)与[账户服务测试](../../../tests/integration/AccountService_unittest.cc)使用真实 SQLite 和密码适配器，覆盖事务失败、权限、分页、条件冲突，以及密码计算期间发生的登录、登出、禁用和资料修改。
