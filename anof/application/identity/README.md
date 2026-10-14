# 身份、账户与读取应用服务

[IdentityService](IdentityService.h) 是 metadata worker 上的同步编排，依赖 MetadataStore、Clock、IdentityIdGenerator、CredentialIssuer 和 InviteTokenCodec，不包含 HTTP、JSON 或 SQL。

- `prepareLogin` 返回自有 PasswordSnapshot 并关闭读事务。调用方在独立密码 worker 验证，再调用 `completeLogin`，在写事务复核启用状态、密码版本、epoch 和用户 revision，检查32个活跃 Session 上限，写 Session 和审计。有效期为七天绝对时间。
- `registerUser` 接受已计算密码，在同一事务中消费邀请、检查人数、创建普通用户、私有空间／根、初始额度和公共贡献者额度、记录审计；私有根计一个节点。失败回滚，不自动登录。
- `execute` 重载实现登出、当前用户、管理员邀请创建／分页／撤销、空间列表、节点及 children 读取。写操作复验授权；失效凭据不能通过重复登出成为成功。只在有状态变化时写撤销审计。
- 读取在同一快照内鉴权、检查空间与祖先可见性，再检查游标绑定；管理员没有他人私有空间旁路。分页响应携带已核实的用户／空间／父目录绑定，不维持跨请求快照。
- `cleanupSessions` 在独立短事务删除最多200条失效 Session；频率由 runtime 限制。

登录 token 和邀请码明文只在确定提交成功后返回；提交未知不自动重试、不返回秘密，也不声称回滚。应用服务不执行密码计算、不保存活跃事务等待其他 worker。

[IdentityCommands](IdentityCommands.h) 包含 typed command、DTO 和 cursor；[IdentityDispatcher](../ports/IdentityDispatcher.h) 是异步端口。来源限制、队列、取消和完成回调由 [IdentityExecutor](../../runtime/IdentityExecutor.h) 承担。输入结构与 HTTP 表示由 transport 处理，服务不替代入口的协议校验。

[真实 SQLite／crypto 测试](../../../tests/integration/IdentityService_unittest.cc) 验证事务回滚、私有空间可见性、分页绑定、版本变化、Session上限与提交未知。测试不是 HTTP 或完整产品验收。

## 账户管理

[AccountService](AccountService.h) 仅依赖 MetadataStore 和 Clock，提供本人显示名、管理员用户分页／详情／启禁和两类额度读写。[AccountCommands](AccountCommands.h) 用 AccountCondition 保留用户或额度资源的种类、ID与revision；认证、管理员权限和目标检查先于条件比较。不提供管理员访问他人私有节点的权限，也不提供角色、登录名或密码代改。

显示名、启禁和额度实际变更才追加类型化审计，审计与状态共享写事务；条件过期即使新值相同也失败，无操作保留版本和时间。禁用递增epoch并撤销全部会话，再启用不复活旧凭据；唯一管理员不可禁用。额度上限不可低于used+reserved，额度计数变更使旧条件失效。写入返回成功前确认commit，未知提交不返回假成功或重试。

`preparePasswordChange` 在短读快照中认证并校验条件，返回自有登录名和密码快照。调用方关闭快照后验证旧密码、计算新hash，再调用 `completePasswordChange`：新事务重新检查Session、条件及密码版本，更新密码、撤销全部会话并写审计。最终认证位于本操作递增epoch之前，避免自身撤销使合法改密失败。旧密码验证失败仍经最终认证，Session失效为Unauthenticated、有效Session下资料版本冲突为PreconditionFailed、旧密码错误为Forbidden；失败不写入。成功没有新token，要求重新登录。

[账户服务测试](../../../tests/integration/AccountService_unittest.cc) 使用真实SQLite／crypto，覆盖管理授权与私有节点隔离、条件绑定／无操作、两类额度及目录计费顺序、改密与登录／改名／禁用／登出／到期的阶段交错、全部会话撤销、审计失败回滚和已提交但确认未知。事务组合验证不等同于HTTP协议验收。
