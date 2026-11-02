# 密码学适配契约

本页供调用或修改 crypto 实现时查阅。业务中的典型调用顺序见 [README](README.md)；这里重点说明哪些输入可接纳、秘密归谁持有，以及失败是否能被解释为正常拒绝。

## 从密码输入到持久化编码

[SodiumPasswordPolicy](SodiumPasswordPolicy.h) 没有有效的默认工作策略。工厂先校验策略，再初始化 libsodium；无效参数返回 `InvalidArgument`，可报告的初始化或分配失败返回 `InternalError`。

| 参数 | 范围和关系 |
|---|---|
| `hashing.memoryKiB`、`verification.maxMemoryKiB` | 8～262144 KiB；转换为字节前检查 size_t 与 libsodium 限制 |
| `hashing.iterations`、`verification.maxIterations` | 1～10 次 |
| `hashing.parallelism`、`verification.maxParallelism` | 固定为 1 |
| 生成参数与接纳上限 | hashing 不得超过 verification |

这些是单次操作的接纳范围，不是进程 RSS 硬上限，也不是推荐密码强度。线程数、排队与内存总预算由装配层控制；适配器不校准参数。SQLite 的 `passwordHashLimits` 应与 verification 一致，否则数据库能读出的编码可能无法验证，或有效编码被读取层拒绝。

[hash](SodiumPasswordHasher.cc) 接受领域对象持有的 12～1024 字节原始 UTF-8 密码，不 trim、归一化或截断。它生成 Argon2id v19 的 PHC 编码，使用独立随机盐，固定 16 字节盐和 32 字节输出，再复验生成的编码和参数，返回 move-only `EncodedPasswordHash`。已经移走秘密的空对象不作为有效输入。

`verify` 在计算前重新检查 PHC 结构、参数和固定盐/摘要长度，不因为编码曾在更宽预算下构造就直接接受。超预算、畸形或此适配器不支持的编码返回 `InvalidArgument`。计算完成后使用 `sodium_memcmp` 比较摘要：不匹配返回 false，算法失败返回 `InternalError`。实现没有把二者合并成“密码错误”。

调用者在计算前结束数据库快照或事务；验证后的账户 revision、epoch、启用状态仍要在最终写事务中复核。dummy hash 的选择、Session 签发和登录响应属于上层，不属于 PasswordHasher。

## 凭据的签发和摘要

[CredentialIssuer](SodiumCredentialIssuer.cc) 的 `issueSession`、`issueInvite` 每次取得 32 个新鲜随机字节，返回自有 `SecretToken` 和强类型摘要。Session 明文是恰好 43 字符的无 padding base64url；邀请增加 `anof_invite_` 前缀，总长 55。摘要均是原始随机字节的 SHA-256，编码文本和邀请前缀不参与哈希。

SessionDigest 与 InviteDigest 是不同类型。相同原始字节的数学摘要可能相同，类型隔离不依赖另一段哈希前缀。适配器不持久化、不定义到期或撤销；应用只有在关联事务确认提交后才能交付明文，提交未知不能当作签发成功。

[SessionTokenCodec](SodiumSessionTokenCodec.cc) 的 `digestBearer` 接收不含 HTTP scheme 的文本视图，仅接受 43 字符规范 base64url：字母表 `A-Z a-z 0-9 - _`，最后字符的低两位必须为零。空白、NUL、padding、额外数据或摘要文本均拒绝；输入不要求 NUL 结尾。

校验后解码器必须消费完整输入并得到恰好 32 字节，再计算 SHA-256。非法文本返回 `Unauthenticated`，已通过规范校验后的解码后置条件异常或哈希失败返回 `InternalError`。每次调用不初始化库、不访问随机源/文件/数据库，也不运行密码算法或进行正常路径堆分配。退出时清理局部解码缓冲区，不保留借用视图。

[InviteTokenCodec](SodiumInviteTokenCodec.cc) 复验自有 InviteTokenText 的长度、前缀及同样的规范编码。非法或已移走的输入为 `InvalidArgument`，内部哈希失败为 `InternalError`。它复用固定长度的解码逻辑，但不把带前缀邀请当作 Session 凭据。

## 请求 ID 与随机实体 ID

[RequestIdentity](SodiumRequestIdentity.cc) 在构造时取得 32 字节随机启动种子，只在对象内保存并在析构时清零。生产工厂不接受固定种子，不持久化或记录种子。实例复用同一个 provider，不按请求重新创建。

`forExchange(connection, request)` 对以下 66 字节作 SHA-256：

| 字节偏移 | 内容 |
|---|---|
| 0～17 | ASCII `ANOF/request-id/v1`，不含 NUL |
| 18～49 | 32 字节启动种子 |
| 50～57 | connection，8 字节大端 |
| 58～65 | request，8 字节大端 |

取摘要前 16 字节，保持其中原有 version/variant 位，按小写 UUID 分组生成 RequestId。仅当前 16 字节全零时，将最后一字节置为 1。编号 0 和 UINT64_MAX 都有效；相同 provider 和编号对得到相同结果。调用只读种子，使用固定栈缓冲，不维护编号映射；可以并发只读调用，但不能与对象销毁并发。

该接口没有 Result 通道：哈希失败抛 `std::runtime_error`，格式化不变量失败抛 `std::logic_error`，消息不含种子或输入。调用者必须处理异常，不能降级为正常外观的 ID。派生缓冲中的种子副本由 RAII 清零。请求 ID 仅用于关联，不保证跨启动绝对无碰撞。

实体 ID 使用不同的生成方式。Node、User、Space、Invite、Instance、Operation、Upload、Blob、Trash，以及维护 Run/Plan/Action/Quarantine/Backup 每次都从新鲜的 16 字节安全随机值生成 UUID v4：第 6 字节高四位设为 `0100`，第 8 字节高两位设为 `10`，其余 122 位保持随机输入。布局保证非 nil，输出为小写 `8-4-4-4-12` 分组。实现见[共享 UUID 格式化](internal/RandomUuid.h)。

生成器不查询数据库、不处理唯一冲突，也不复用 RequestIdentity 的种子或计数器。唯一性冲突由持久化约束发现后，交给上层决定处理方式。[SodiumMaintenanceDigest](SodiumMaintenanceDigest.h) 则仅对传入的受限计划编码计算 SHA-256，不生成 ID、不读写文件或保留输入。

## 初始化与秘密清理

所有需要 libsodium 的工厂先调用共享初始化。初始化可能等待随机源，没有截止时间保证；库级致命终止不能转成 Result 或通过 C++ 异常恢复，不退回固定种子或较弱随机源。不要把这条初始化路径放到每次请求的摘要计算中。

适配器不记录密码、编码、token 或摘要。自有盐、原始 token、PHC 输出及计算临时缓冲由 RAII 清理；返回秘密由领域 move-only 对象持有并在析构清零。调用方输入、寄存器、分页及第三方库内部副本不在这一保证内，也没有 mlock 保证。

内部故障接缝用来验证初始化、计算、编码和异常分类，不替换生产全局 RNG，也不是公开的认证替代入口。相关回归导航见 [README](README.md#修改时检查什么)。
