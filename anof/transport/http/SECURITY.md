# 凭据与同源保护

HTTP 入口要先确认请求从哪里来，再决定它使用哪种凭据。这样，浏览器自动附带的 Cookie 不会绕过来源检查，反向代理也不会把所有用户的限流来源混成同一个地址。本文说明 [BrowserSecurity](BrowserSecurity.cc)、[RequestCredentials](RequestCredentials.cc) 和登录响应的配合；模块装配见 [README](README.md)。

## 配置可信入口

HttpApi 构造时复制所需配置并检查组合，非法值抛出固定消息的 `invalid_argument`，不会降级为不安全模式。监听地址只允许 `127.0.0.1` 或 `::1`。

| 模式 | 必要配置 | 请求来源 |
| --- | --- | --- |
| 本机开发 HTTP | `developmentHttp=true`，关闭 Web，Origin 和代理列表为空 | 使用 socket peer；转发身份头不能覆盖它 |
| HTTPS 同机反代 | `developmentHttp=false`，规范 HTTPS publicOrigin，1～2 个不同的精确 loopback 代理地址 | peer 必须在代理列表中，再使用经过校验的转发 IP |

正式配置即使只供 Bearer 客户端使用，也要求 Origin 和可信代理；只有 `webEnabled=true` 才允许 Cookie。`publicOrigin` 使用 [PublicOrigin](../../domain/identity/PublicOrigin.h) 规则：最多 256 字节，DNS 小写、默认 443 端口省略、IPv6 带方括号，不含 userinfo、路径、尾部斜线、query 或 fragment。配置值本身必须已经规范化。部署文件和 TLS 设置见[部署说明](../../../deploy/README.md#配置-anof)。

正式请求的 `Host` 必须精确等于配置的 authority，也就是域名或地址及必要端口。可信 peer 必须提供唯一的 `X-Forwarded-For`（无端口、无列表、规范 IP）和 `X-Forwarded-Proto: https`。缺失或非规范转发信息返回 400；非可信 peer 带转发信息也返回 400，不带则正式入口返回 403 `REQUEST_ORIGIN_REJECTED`。`Forwarded`、`X-Real-IP` 和其余 `X-Forwarded-*` 一律拒绝。来源不能从客户端正文或 Host 推导。

这些检查也作用于健康路由；健康请求不需要业务身份，但正式模式仍需经过配置的代理。有效来源作为自有 `AuthenticationSource` 传给登录、注册和改密命令，供 runtime 限流。缺失或无法解析的连接 peer 属于入口内部错误，不使用默认地址替代。

## 解析凭据

Cookie 解析先处理头部歧义，再提取会话值。只允许一个 Cookie 头，总值最多 8192 字节、最多 64 个 `name=value` 对；按分号分隔，允许对两端的空格和制表符。名称须符合 HTTP token 字符规则，拒绝控制字节、值内部空白、任意重复名称和不完整的键值对。框架的头部限制可能更早以 431 拒绝。

只有大小写精确的 `__Host-anof_session` 表示会话。其他语法合法的 Cookie 被忽略，不影响 Bearer 请求。会话值必须原样为规范 43 字节 base64url；不去引号、不做百分号解码，空值或非规范值返回 401。

| 请求中的凭据 | 业务入口的处理 |
| --- | --- |
| 唯一 Authorization，无会话 Cookie | 按 Bearer 解析；缺少 token、非法格式或摘要失败返回相应错误 |
| 唯一规范会话 Cookie，无 Authorization | 在 Cookie 模式启用且来源检查通过后，使用 Cookie 模式 |
| Authorization 与会话 Cookie 同时存在 | 400 `INVALID_ARGUMENT`，即使其中一个值无效也不选择优先身份 |
| 重复 Authorization 或 Cookie 语法歧义 | 400 `INVALID_ARGUMENT` |
| 两者皆无 | 受保护路由返回 401；健康和注册／登录不要求已有 Session |

Bearer 方案名比较不区分 ASCII 大小写；分隔方案和 token 的位置要求空格，token 内不能含空白或逗号。两模式均通过 `SessionTokenCodec.digestBearer` 对裸 token 做规范解码和摘要。返回的 `RequestCredentials` 包含 digest 与独立的模式；业务请求把二者放入 `RequestContext`。真正的会话有效期、撤销、用户状态及 digest＋mode 匹配由应用和 repository 验证，不能因为 transport 解码成功就认为已授权。

Cookie 解析器返回的视图仅供同步调用。选择器结束前把必要内容转换为自有类型，异步状态不保存原始 Cookie、Authorization 或 token。

## Origin 和请求确认头

这里把 GET、HEAD 视为安全方法，其他方法均需考虑跨站写入。对所有非安全 Cookie 请求，以及任何携带 `Origin` 或 `X-ANOF-Request` 的非安全请求，入口要求：

1. 唯一 Origin 按 PublicOrigin 规则解析并规范化后，与配置的 Origin 相等。
2. 唯一 `X-ANOF-Request` 去掉两端空格／制表符后，值恰为 `1`。

缺少或错误 Origin 返回 403 `REQUEST_ORIGIN_REJECTED`；Origin 已通过但确认头缺少或值错误，返回 403 `CSRF_REQUIRED`。重复头或多值歧义返回 400。未启用的 Cookie 业务模式返回 403 `COOKIE_MODE_DISABLED`。GET／HEAD 可省略 Origin，以支持直接下载；若提供仍须匹配。模块不开放跨域 CORS 或自动 OPTIONS。

`BrowserSecurity` 先检查 Cookie／头部歧义和来源，再执行 Origin／CSRF 检查；通过后才进入 HTTP 容量、路由和业务凭据处理。来源或语法错误因此可能先于 401、404、405 或业务条件错误返回。安全预检不查询数据库，也不替代随后由应用执行的授权。

## 登录与注册

登录正文必须含 `credential_mode`，值为 `bearer` 或 `cookie`。注册同字段可省略，默认为 bearer；注册成功只返回用户资料，不签发 Session。两者都拒绝任何 Authorization。Bearer 匿名请求遇到会话 Cookie 返回 400，Cookie 登录／注册则必须先通过同源保护。

Cookie 模式可以携带旧会话，但仍检查 token 编码。非规范旧值返回 401，不发清除头。规范但已过期、撤销或数据库中不存在的旧值，不作为身份验证，也不妨碍用密码重新登录；该请求不会因此撤销其他 Session。模式位于正文中，匿名入口只有收齐并解析合法正文后才能完成这一模式检查。

Bearer 登录成功 JSON 保留 `user`、`expires_at`、`token_type`、`access_token`。Cookie 登录成功 JSON 只有 `user`、`expires_at`、`credential_mode="cookie"`，秘密通过响应头交付：

```text
Set-Cookie: __Host-anof_session=<新会话值>; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=604800
```

这里是头部形状说明，`<新会话值>` 不是可用凭据。Cookie 没有 Domain；服务端到期时间仍以 `expires_at` 为准。登录和邀请创建在转换时先序列化非秘密字段，再把秘密直接加入最终正文或头部，不进入通用 JSON DOM、日志、异常或 URL。响应缓冲的清理范围见[生命周期说明](STREAMS.md#收体与秘密缓冲)。

## 哪些响应清除 Cookie

清除策略绑定到每次请求的响应头，使用同名空值及相同安全属性，另设 `Max-Age=0` 和 `Expires=Thu, 01 Jan 1970 00:00:00 GMT`。清除旧 Cookie 与事务是否成功是两个不同结果。

| 情况 | 行为 |
| --- | --- |
| Cookie 注销／改密已通过安全检查且会话值格式合法 | 对可构造的最终响应清除，包括成功、401、业务拒绝、提交未知、过载、ready 拒绝和选择器后的框架超时／正文限制 |
| 无 Cookie、无 Authorization，但注销显式通过同源保护 | 可返回 401 并清除 |
| Cookie 登录正文与模式检查通过 | 失败响应清旧 Cookie，成功响应设置新 Cookie |
| Cookie 语法、来源、Origin 或 CSRF 未通过 | 不因该请求清除 Cookie |
| Bearer 响应 | 不发送 Set-Cookie |

登录在完整合法正文解析前被拒绝，例如 HTTP 准入、ready 或收体期限拒绝，尚不能确认模式，因此不承诺清除。选择器之前的 HTTP 解析、头部大小或头部超时错误也没有每请求清除状态。普通 Cookie 业务 401 不额外自动清 Cookie。

断连、writer 中止或响应构造异常可能使清除头无法到达浏览器。客户端应提示重新同源注销或登录，不能声称 HttpOnly Cookie 已被 JavaScript 删除，也不能把发送失败解释为登录事务回滚。
