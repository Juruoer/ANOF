# 身份领域值

身份领域值让入口和业务使用同一套账户、会话和秘密类型。例如，登录名在这里统一大小写，密码则原样保留；读取数据库或 HTTP 请求后，都需要先得到这些受检值。实际鉴权和密码计算由应用与适配器完成。

## 账户与文本规则

[IdentityValues.h](IdentityValues.h) 定义登录名、显示名、凭据模式、独立邀请摘要及受检数量。登录名为 3～32 个 ASCII 字节，首字符字母，其余可含数字、下划线、连字符；大写规范为小写，不裁剪空白。显示名为 1～128 个有效 UTF-8 字节，不含 NUL 或 C0/C1 控制字符。分页大小 1～200、用户上限和邀请使用上限 1～10000，密码版本至少为 1。

## 密码和令牌的寿命

[Secrets.h](Secrets.h) 中密码、token、邀请码输入、编码哈希均不可复制，只可移动。秘密使用独立堆缓冲，移动移交分配，覆盖/析构清零自身缓冲；view 只供同步借用，不能在所属对象移动或销毁后保留。不保证清除调用者、解析器、寄存器或分页副本，不提供通用日志/JSON 序列化。

密码为 12～1024 个有效 UTF-8 字节，不含 NUL，不规范化或裁剪；控制字符不会被悄悄删除。Session token 为 32 字节数据的 43 字符规范无 padding base64url；邀请文本在其前加 `anof_invite_`。格式检查不证明随机性、摘要对应关系或凭据有效性。

`EncodedPasswordHash::parse` 检查最长 512 字节的 Argon2id v19 PHC 结构，显式接受 `PasswordHashLimits`。m/t/p 必须为正整数且不超过预算，m 至少为 8p；盐和哈希分别为 16～64 字节的规范无 padding base64。此检查不计算密码、不校准预算，也不保证所有 crypto 后端都支持该结构范围；实际适配器须按选定算法策略进一步约束。

## 会话、模型与事件

[User.h](User.h)、[Session.h](Session.h)、[Invite.h](Invite.h) 将公开用户属性与秘密密码快照分离。UserProfile 中 epoch 等内部字段不应直接全部序列化为 HTTP 用户表示。`UserListPosition` 仅保存用户 ID，供按规范 ID 升序的 seek 分页使用，不含游标编码或权限。Session 与 Invite 各自只向持久化模型提供摘要。这些类型检查各自的字段。用户、会话和邀请之间是否匹配，仍由应用和仓储检查。

[IdentityEvent.h](IdentityEvent.h) 使用 variant 要求事件对应的归属字段，不接受任意 action 或秘密载荷。领域值不负责事件持久化。

## 网站地址

[PublicOrigin.h](PublicOrigin.h) 解析有界 HTTPS Origin，规范 DNS 大小写、IPv6 和默认 443 端口；拒绝路径、userinfo、查询/片段和歧义数字主机。配置校验要求保存规范值；请求可在解析后比较规范值。 可信代理值仅接受精确 `127.0.0.1` / `::1`，不支持 CIDR。这些值校验不执行 HTTP 头或 CSRF 检查。

## 测试

[IdentityValues_unittest.cc](tests/IdentityValues_unittest.cc) 覆盖大小写、UTF-8/NUL/控制字符、字节边界、秘密移动、规范 base64 尾位、PHC 参数预算及整数溢出。所有测试使用内存小样本，不执行密码哈希或资源压力测试。
