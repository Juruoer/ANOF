# HTTP 接入层

`anof_http` 为创建目录、健康检查、身份、邀请、账户管理、目录读取、上传、下载、文件管理、搜索、回收站及实例存储管理操作提供 HTTP 路由、协议转换、请求关联及异步生命周期适配。模块只依赖 application/domain 类型和抽象端口、muduox 的 HTTP/router/middleware 公共接口，以及实现内部使用的 nlohmann-json。它不直接访问数据库、调用同步应用用例或创建 worker。当前实际注册下表中的四十一个 HTTP 操作。

## 接口与装配

| 接口 | 职责 |
|---|---|
| [TransferRequest](TransferRequest.h)／[TransferResponse](TransferResponse.h) | 上传／storage 输入、DTO、头与错误转换 |
| [DriveRequest](DriveRequest.h)／[DriveResponse](DriveResponse.h)／[DriveHandlers](DriveHandlers.h) | 文件管理条件与 JSON、搜索／回收站表示、八个异步入口 |
| [DownloadResponse](DownloadResponse.h)／[DownloadStream](DownloadStream.h) | 授权 open 后的条件／Range、表示头与单块异步下载发送 |
| [UploadStream](UploadStream.h) | PATCH 异步准备、单缓冲 feed／finish 与取消 |
| [HttpApi](HttpApi.h) | 配置路由、HTTP 容量限制、响应头策略与交换 observer |
| [CreateDirectoryHandler](CreateDirectoryHandler.h) | 收体、命令校验、异步提交、结果发送和取消清理 |
| [RequestTrace](RequestTrace.h) | 通过注入的 RequestIdentity 关联请求 ID 与 HTTP 提交／终态 |
| [readBearerCredentials / validateJsonContentHeaders](BearerCredentials.h) | 提取 Bearer、检查认证来源与内容头 |
| [parseCreateDirectoryJson](CreateDirectoryJson.h) | 有界 SAX 解析，返回自有 CreateDirectoryCommand |
| [JsonResponse](JsonResponse.h) | 成功表示、业务／提交错误映射、时间格式和有限 JSON 响应 |
| [makeIdentityStream](IdentityStream.h) | 类型化控制任务桥接、公共异步请求状态及有界分块响应 |
| [AccountResponse](AccountResponse.h) | 管理用户、用户页、管理额度及额度更新错误转换 |
| [AccountRequest](AccountRequest.h) | 账户条件标签、路径／额度作用域及账户写入 JSON 的自有命令解析 |
| [IdentityRequest](IdentityRequest.h) | 注册／登录／邀请 JSON、路径 ID、匿名凭据、无正文输入及可信 peer 校验 |
| [Pagination](Pagination.h) | 分页查询、绑定游标编解码及严格 UTC 解析 |
| [IdentityResponse](IdentityResponse.h) | 用户、登录、邀请、空间、节点及分页表示，204 无正文响应 |

调用方提供目录 dispatcher、IdentityDispatcher、codec、identity、内存 ready 回调、TransferDispatcher、DriveDispatcher 及观测 sink。IdentityDispatcher 是必需的注入依赖，十九个身份／邀请／账户／读取操作通过它提交类型化异步任务。handler 和 HttpApi 应共享同一个生命周期来源的 ready 回调。以下代码在 server 所属 loop 执行：

```cpp
CreateDirectoryHandler handler(dispatcher, ready, taskObserver);
HttpApi api(codec, identity, ready, handler.stream(), identityDispatcher, transferDispatcher, driveDispatcher, httpObservation);
api.configure(server);
server.start();
```

`configure` 在 `start` 前仅调用一次，配置主 loop 处理连接（额外 I/O 线程数为 0），一次安装组合 selector、全局 observer 和响应头策略。后续不能用其他 HTTP setter 覆盖它。`HttpApi` 不创建、启动或停止 server。

`handler.stream()` 和安装到 server 的回调按值保留配置，不借用 handler/HttpApi 对象本身；dispatcher、codec、identity 及用户回调引用的资源仍须保持存活。调用方先关闭业务准入，再在主 loop 调用 `HttpServer::stop`，继续驱动 loop，等待实际 HTTP 清理通知和外部任务全部完成之后才能销毁依赖和 loop。HTTP 清理完成不代表整个 runtime 停止。

## 路由与准入

| 端点 | 行为 |
|---|---|
| `PATCH /api/v1/nodes/{node_id}` | 改名／移动或组合更新，200 NodeMetadata＋ETag |
| `POST /api/v1/nodes/{node_id}/copy` | 单文件复制，201 NodeMetadata＋ETag＋node Location |
| `DELETE /api/v1/nodes/{node_id}` | 移入回收站，200 TrashEntry＋ETag＋trash Location |
| `GET /api/v1/search` | 按空间和字面名称查询一页活动节点 |
| `GET /api/v1/trash` | 按空间、可选原根 ID 查询一页回收站条目 |
| `GET /api/v1/trash/{trash_id}` | 条目详情，200 TrashEntry＋ETag |
| `POST /api/v1/trash/{trash_id}/restore` | 恢复，200 NodeMetadata＋ETag＋node Location |
| `DELETE /api/v1/trash/{trash_id}` | 永久删除确认，204；不表示物理回收完成 |
| `GET /api/v1/nodes/{node_id}/content` | 授权 open 后发送完整内容／单范围，支持条件请求 |
| `HEAD /api/v1/nodes/{node_id}/content` | metadataOnly open，返回完整表示头，无正文、无流式槽 |
| `POST /api/v1/uploads` | 创建／重放上传，按返回 disposition 为 201／200／202 |
| `GET /api/v1/uploads` | 上传分页或 client_upload_id 精确筛选 |
| `GET /api/v1/uploads/{upload_id}` | 查询自有上传及保留终态 |
| `HEAD /api/v1/uploads/{upload_id}` | 200 仅上传头，无正文／Content-Type／Content-Length |
| `PATCH /api/v1/uploads/{upload_id}` | 1～8 MiB 原始字节，finish 确认后 204＋Upload-Offset |
| `DELETE /api/v1/uploads/{upload_id}` | 逻辑取消确认 204，不代表磁盘清理完成 |
| `POST /api/v1/uploads/{upload_id}/complete` | 接纳后台校验／发布 202，已完成 200 |
| `GET /api/v1/admin/storage` | 预算、计量、独立 ETag 与磁盘采样 |
| `PUT /api/v1/admin/storage` | 强 If-Match 条件更新预算，200 |
| `POST /api/v1/nodes` | 检查 ready、Bearer 和内容头，将自有 RequestContext 绑定到注入的流式处理器 |
| `GET /health/live` | 返回 `{"status":"live"}`，不访问业务存储 |
| `GET /health/ready` | ready=true 时返回 `{"status":"ready"}`，否则为 503 错误 envelope |
| `POST /api/v1/auth/register` | 匿名注册，201 用户资料，不创建 Session |
| `POST /api/v1/auth/sessions` | 匿名登录，201 Bearer token、到期时间及用户资料 |
| `DELETE /api/v1/auth/session` | 登出当前 Session，成功 204 |
| `POST /api/v1/admin/invites` | 创建邀请，201 元信息及一次性明文 token |
| `GET /api/v1/admin/invites` | 邀请元信息分页，包含已撤销／失效项 |
| `DELETE /api/v1/admin/invites/{invite_id}` | 撤销邀请，成功或已撤销为 204 |
| `PATCH /api/v1/users/me` | 修改本人显示名，200 用户资料及 ETag |
| `POST /api/v1/users/me/password` | 改密成功 204，无新 token／ETag |
| `PATCH /api/v1/admin/users/{user_id}` | 启禁用户，200 管理用户详情及 ETag |
| `PUT /api/v1/admin/spaces/{space_id}/quota` | 更新空间额度 limit，200 管理额度及 ETag |
| `PUT /api/v1/admin/spaces/{space_id}/contributions/{user_id}/quota` | 更新贡献额度 limit，200 管理额度及 ETag |
| `GET /api/v1/admin/users` | 管理用户分页，无集合 ETag |
| `GET /api/v1/admin/users/{user_id}` | 管理用户详情与私有空间 ID，带返回用户 ETag |
| `GET /api/v1/admin/spaces/{space_id}/quota` | 空间额度，user_id 为 null，带额度 ETag |
| `GET /api/v1/admin/spaces/{space_id}/contributions/{user_id}/quota` | 用户贡献额度，带绑定空间和用户的额度 ETag |
| `GET /api/v1/users/me` | 当前用户资料及用户 ETag |
| `GET /api/v1/spaces` | 本人私有空间和公共空间及独立额度 |
| `GET /api/v1/nodes/{node_id}` | 活动可见节点元数据，头部／正文 ETag 一致 |
| `GET /api/v1/nodes/{node_id}/children` | 目录直接子项分页及绑定游标 |

未知路径返回 404；已知路径的方法不匹配返回 405 和 Allow。上传详情和文件内容注册显式 HEAD；HEAD fallback、自动 OPTIONS 和 CORS 关闭。健康检查不调用凭据 codec，也不查询数据库。健康路由的正文限额为 0。

`HttpApiOptions` 的 `maxInFlight` 默认 128，允许 1～128；`bodySeconds` 默认 15、`responseSeconds` 默认 30，只允许设置更短的正有限期限。`patchBodySeconds` 默认 300、`patchResponseSeconds` 默认 60，也只允许收紧。configure 显式设置全局 bodyBytes=8 MiB、输出高水位 1 MiB，期限 headers=15、idle=60、body=300、response=21600、write=3600、close=5 秒。JSON／控制路由收紧为原 bodySeconds／responseSeconds，PATCH 使用独立期限，内容路由使用 contentResponseSeconds（默认 21600 秒）。所有 JSON 写操作正文最多 65536 字节，实际 trailer 在 complete 后拒绝。路由属性与原决定合并，保留更严格的限制；server 默认解析限制及阶段期限继续生效。

受保护路由要求 Bearer，拒绝任何 Cookie；GET／DELETE 禁止非零 Content-Length、Transfer-Encoding 和 trailer。children／邀请列表／管理用户列表只允许唯一 limit／cursor，其余身份／邀请／账户／读取操作不接受查询参数。选择阶段先校验凭据格式，再把路径和查询转换为自有类型化命令，提交在完整输入回调执行。同步层不查询数据库或验证业务权限；Session、可见性和游标用户／空间／父目录绑定由应用判断。应用返回的未认证、不可见及可见非目录父节点分别映射 401、404、409。元数据 JSON GET 不实现条件缓存，即使携带 If-None-Match 也正常提交并返回完整表示；内容 GET／HEAD 在授权 open 后求值条件。

身份／邀请／账户／读取路由沿用业务 15 秒正文／30 秒响应期限和全局准入限制。注册／登录在选择阶段拒绝任何 Authorization／Cookie，复制可信连接 peer 的来源值，不采信 Forwarded／X-Forwarded-For／X-Real-IP；只携带服务端 RequestId 提交匿名命令。其余操作携带 Bearer RequestContext，管理员资格、邀请码可用性和 Session 状态由应用检查。登录与邀请创建只在成功完成时序列化秘密，错误和迟到结果不泄露 token；两者不设置 Location 或 Cookie。

动态路由使用 muduox 的 `:node_id`／`:invite_id`／`:user_id`／`:space_id` 语法，保留原有 mkdir 路由及其 POST 方法策略。

全局 InFlightLimiter 同时限制健康检查和业务请求。名额在交换 finished 或最后一个属性副本释放时归还，与 worker 的任务名额分开。临时容量耗尽返回 503，不改变 ready 标志。头部拒绝采用 respond + Close，不等待正文，不发送 100 Continue。100 只表示允许继续传正文，不代表 Session 已经通过业务鉴权。

## 输入与线上表示

凭据提取拒绝重复 Authorization 或任何 Cookie；缺少、格式无效的 Bearer 返回 401。只将 token 文本传给 [SessionTokenCodec](../../application/ports/SessionTokenCodec.h)，规范 base64url 校验及摘要算法由该端口的具体实现负责。模块不验证 Session 状态，不记录 token、摘要或原始认证头。内容类型只接受 application/json 和可选唯一 UTF-8 charset，Content-Encoding 只允许缺省或单个 identity；重复歧义内容头返回 400，不支持的媒体类型或编码返回 415。

JSON 解析不构造 DOM。创建目录输入必须是含 space_id、parent_id、name 三个字符串的一层对象；缺失、未知、解码后重复键、非字符串值、额外 JSON 值或非法 Unicode 均拒绝。嵌套对象或数组在开始事件立即拒绝，比通用深度 32 上限更早终止。ID 使用已有强类型解析器；名称复用领域规则，限制为 1～255 UTF-8 字节，不 trim、折叠大小写或归一化。借用的头部和正文只在调用期间使用，返回值拥有自己的数据。

创建目录成功对象只包含 id、space_id、parent_id、name、kind、created_by、created_at、updated_at。时间固定 UTC 六位微秒，支持公历年份 0001～9999，正确处理 epoch 之前的负微秒。Location 为 `/api/v1/nodes/<id>`，ETag 为带双引号的 `"node-<id>-1"`。父 ID 缺失、revision 非 1、创建和更新时间不一致、名称或时间无法表示时返回 InternalError；不能据此认定已经提交的事务被撤销。

所有模块生成的 JSON 响应带 application/json、Cache-Control: no-store、X-Request-ID；错误体使用同一请求 ID，details 固定为空对象，401 另带 Bearer challenge。应用错误与协议错误集中映射，CommitOutcomeUnknown 保留独立 code；模块不自动重试。框架错误、HEAD 和连接中止不保证 JSON，不能无条件解析响应正文。

`JsonResponse` 将响应头和正文分开保存，不自行设置 Content-Length 或 Transfer-Encoding；`buffered(close)` 生成有限缓冲响应。异步 handler 将已准备的正文和确定长度交给 HttpStreamSender，producer 的 nullopt 仅表示 EOF。

## 身份与读取协议函数

`IdentityRequest` 返回自有类型化命令或 `ProtocolError`。注册／登录只接受规定的一层字符串字段，邀请创建只接受整数参数；空对象使用 1 次、604800 秒默认值。SAX 在嵌套开始时拒绝，检查解码后的重复键和 64 KiB 正文上限，字段值复用领域校验。登录名转 ASCII 小写，密码不 trim 或归一化。解析器清零其持有的字段字符串，成功命令使用只移动的秘密类型；这不保证擦除调用者正文、lexer 或系统中的其他副本。

身份写请求的字段约束如下，所有字段均拒绝未知键和解码后重复键：

| 请求 | 输入 |
|---|---|
| 注册 | 必填字符串 `login`、`display_name`、`password`、`invite_token`；密码为 12～1024 UTF-8 字节，显示名为 1～128 UTF-8 字节 |
| 登录 | 必填字符串 `login`、`password`、`credential_mode`；模式只能为 `bearer`，密码不 trim |
| 创建邀请 | 可选整数 `max_uses`（1～10000，默认 1）、`valid_for_seconds`（1～2592000，默认 604800）；必须提供 JSON 对象，空正文不等于 `{}` |

匿名凭据校验拒绝任何 Authorization／Cookie。无正文校验分为初始定界检查和完整输入检查，后者拒绝实际正文／trailer；调用者负责关闭被拒绝的交换。`readAuthenticationSource` 只从 metadata 的 peer 按声明的 IPv4／IPv6 family 解析，忽略端口；IPv4 和对应 mapped IPv6 值相等。缺失或非法 peer 返回 InternalError，没有默认地址或转发头入口。

分页查询仅接受唯一的 limit／cursor；limit 默认为 100，范围 1～200，拒绝前导零、符号、小数、指数和空值。游标为最多 2048 字符的规范无 padding base64url，解码最多 1536 字节，检查版本、形状、规范小写 UUID、非根名称或严格 UTC 六位小数。结构解析不比较用户、空间或目录绑定，不访问数据库；调用方必须通过应用层在授权后核对绑定。响应游标直接编码 dispatcher 返回的绑定值。

用户表示排除 auth epoch、密码版本和摘要；me 带用户 ETag，注册仅返回用户资料。节点表示区分空名／无父节点的根、普通目录和含大小／SHA-256 的文件，节点 GET 的头部 ETag 与正文一致。空间列表恰两项，按 ID 排序，检查 private/public 的 owner 和贡献额度字段形状；本人身份及可见性由应用保证。数量和 revision 使用受检整数的十进制字符串，邀请次数仍为 JSON 整数。

登录／邀请创建转换消费只移动的秘密结果：先校验和序列化非秘密字段，再将经过格式验证的 token 直接追加到最终正文，不放进通用 JSON DOM。只有成功创建响应包含明文；调用方须在发送或取消终态清理响应正文。204 响应无正文、无 Content-Type，仍带 no-store 和请求 ID。分页最多 200 项，整份 JSON 存储上限为 512 KiB，允许超过 64 KiB；发送方必须按 writer 的单块限制分块。协议转换单元测试不验证异步发送生命周期；公共桥接另有独立 loopback 测试。

错误映射区分 `409 SESSION_LIMIT_REACHED`、`429 RATE_LIMITED` 与 `503 TEMPORARILY_UNAVAILABLE`。新增业务错误均有显式映射，提交未知保持 `500 COMMIT_OUTCOME_UNKNOWN`，不暗示回滚。账户写入使用 412／428 条件错误，首批写入不增加 If-Match 要求。

## 账户输入协议函数

[AccountRequest](AccountRequest.h) 提供独立输入转换函数，当前路由表包括上述三十一个操作。`parseAccountCondition` 从唯一 If-Match 读取强标签，支持 user、quota-space、quota-contribution 三种绑定。只去除外围空格／制表符；标签内 UUID 必须非 nil、规范小写，revision 为 1～INT64_MAX 规范十进制。缺失返回 428，空值、重复头、弱标签、列表、通配符、非法编码或溢出返回 400。返回完整 AccountCondition；合法但与路径种类／ID 不匹配的条件仍保留给应用授权后的绑定检查，解析器不比较当前资源或 revision。

五个账户写入入口均要求 Bearer、JSON、无查询参数和一个强 If-Match；条件标签在选择阶段复制，完整正文解析后提交。合法错配条件不会被 transport 提前比较或替换。改密来源复用可信 socket peer 解析，不信任转发头；成功 204 不登录或签发 token。旧密码错误的 403、最终 Session 失效的 401 和版本变化的 412 由应用完成结果分别映射，不自动重试，也不宣称断开意味着回滚。

改名只接受 display_name 字符串，改密只接受 current_password／new_password 字符串，启禁只接受 enabled 布尔值。字段值复用领域校验；改密命令只移动，拥有两份 PasswordInput 及调用方在选择阶段解析的可信 AuthenticationSource，不读取客户端来源字段。平坦解析器仅对声明为布尔类型的字段接纳布尔值，原字符串／整数协议保持严格类型要求。

额度解析只接受 `{limit:{bytes,nodes}}` 的两层对象，两项均为 0～INT64_MAX 规范十进制字符串；不接受部分更新、JSON 数字、前导零、符号、空白或指数。专用 SAX 解析器检查两层的解码后重复键与未知键，并拒绝额外容器层级和尾随值，不构造 DOM。所有账户正文仍受 64 KiB 限制。`parseGetUser`／`parseQuotaScope` 仅把路径转换为自有 ID／scope，不查询用户、空间或权限。

## 账户响应与用户分页函数

[AccountResponse](AccountResponse.h) 的管理用户响应为 `{user,private_space_id}`，管理额度为 `{space_id,user_id,quota}`，空间额度的 user_id 为 null。用户／额度 JSON 与首批响应共用内部转换，不暴露 epoch、哈希或凭据。200 的用户／额度 ETag 仅根据返回资源及其 revision 生成；本人改名复用 currentUserResponse，改密复用无正文、无 Content-Type／ETag 的 noContentResponse。

账户管理的四个 GET 入口要求 Bearer 且拒绝 Cookie，初始定界和完整输入均禁止正文。只有用户列表接受 limit／cursor；其他管理读取拒绝任何查询参数。选择阶段只保存自有用户 ID／额度 scope，完整输入后提交类型化任务；管理员授权、贡献 scope 是否可用、目标是否存在及数据完整性均由应用检查。管理读取不会调用节点查询或授予私有节点访问权。GET 不要求 If-Match，也不因 If-None-Match 返回 304。

用户列表复用有界分页查询：limit 默认 100、范围 1～200。UserCursor 使用 `{v:1,kind:"users",user_id,after_id}` 的规范 base64url 编码，沿用 2048／1536 字节限制和严格 JSON 校验；解析不比较管理员绑定，不查询数据库。用户页最多 200 项，无集合 ETag；末页 next_cursor=null，空页不能携带下一页游标。编码返回游标前核对 after_id 等于最后一项 ID，保留应用返回的 user_id；排序、唯一性与授权由应用负责。

quotaUpdateError 仅供额度 PUT 的已接纳任务业务错误转换使用，将 QuotaExceeded 映射为 409 QUOTA_EXCEEDED；默认映射及 mkdir 保持 403。其他业务错误沿用公共映射，包括异步 429、412、IntegrityError 的 500 INTERNAL_ERROR 和 COMMIT_OUTCOME_UNKNOWN。错误不回显当前值／ETag。

| 条件／错误 | HTTP 与错误 code |
|---|---|
| 缺少 If-Match／标签语法错误 | 428 PRECONDITION_REQUIRED／400 INVALID_ARGUMENT |
| Unauthenticated／Forbidden／NotFound | 401 UNAUTHENTICATED／403 FORBIDDEN／404 NOT_FOUND |
| PreconditionFailed | 412 PRECONDITION_FAILED，不返回当前 ETag |
| SubmitError::RateLimited／ErrorCode::RateLimited | 均为 429 RATE_LIMITED |
| SubmitError::QueueFull／Stopping | 503 TEMPORARILY_UNAVAILABLE |
| 额度 PUT 的 QuotaExceeded／mkdir 的 QuotaExceeded | 409 QUOTA_EXCEEDED／403 QUOTA_EXCEEDED |
| SessionLimitReached | 409 SESSION_LIMIT_REACHED |
| IntegrityError／CommitOutcomeUnknown | 500 INTERNAL_ERROR／500 COMMIT_OUTCOME_UNKNOWN |


## 身份公共异步桥接

`makeIdentityStream<Command, Value>` 接收类型化 dispatcher（IdentityDispatcher 或 TransferDispatcher）、自有 RequestId 或 RequestContext、正文种类、parser、formatter、ready 回调和可选 mapError 函数，生成 muduox 流式入口。parser 同步返回自有命令或协议错误；formatter 只在活动交换收到对应任务成功结果时调用，void 结果支持 204。mapError 只处理已接纳任务的业务错误，同步提交拒绝保持公共映射。路由选择仍须完成凭据／初始定界检查并安装正文限额与 15 秒／30 秒期限；此函数不注册路由或管理服务启动。

每次流式入口创建独立状态。JSON 正文采用固定 64 KiB 容量，避免扩容留下旧正文副本；解析／提交返回、拒绝、取消及析构时清零。无正文模式不分配正文空间并拒绝实际正文，complete 拒绝 trailer。输入只完成一次，提交在 owning loop 执行，拒绝不等待 completion；接纳后保存句柄并校验完成任务 ID。完成闭包仅弱引用状态，不持有 writer；HTTP 已结束时直接销毁迟到结果，不调用成功 formatter，不序列化迟到 token。

响应先校验 512 KiB 上限，再复制到自有固定缓冲并清零临时正文；producer 每次最多取 writer 的 64 KiB，取出的区间立即清零，取消清零其余部分。HttpStreamSender 负责 acceptedBytes、部分接受、WouldBlock 和 writable 恢复。缓冲存储随请求终态清理。清零保证仅覆盖 transport 管理的缓冲，不能保证 JSON lexer、muduox sender／连接缓冲或内核中的副本被安全擦除。

取消只尝试 cancelBeforeStart，不强制中断最终事务，也不把 HTTP 清理等同于 worker 退出。结果转换或异步发送发生异常时显式 abort；接纳前 parser／ready／submit 异常同样终止交换，不保证 JSON 错误体。已接纳后抛异常且不给句柄属于 dispatcher 违约，不在支持范围内。现有 mkdir handler 保持自身带事务诊断的完成接口。

测试夹具 [IdentityDispatcherFixture](tests/IdentityDispatcherFixture.h) 实现十七个类型化提交重载，用 Queued／Password／FinalTransaction／Ready／Delivered 门控阶段模拟取消语义。结果总是延后交付，名额在 callback 返回后归还；同步拒绝无回调，句柄析构不取消。它不创建密码线程或数据库事务，不能代替真实 executor 验收。

## 异步状态与观测

每请求状态由框架流式回调持有，包含自有上下文、有界正文、任务句柄、writer 和 sender。完整输入只处理一次：拒绝 trailer、检查 ready 和解析命令，随后最多提交一次任务。同步 QueueFull/Stopping 返回 503，RateLimited 返回 429，InternalError 返回 500，拒绝后没有完成回调。已接纳任务完成时返回的 ErrorCode::RateLimited 同样映射 429。

接纳后的完成闭包只弱引用请求状态，独立保存请求 ID 和业务 observer，不捕获 writer。完成在所属 loop 转换和发送；CancelledBeforeStart 在 HTTP 仍活动时返回 503。sender 独占 writable 回调并处理部分接受和 WouldBlock，请求状态保留它至结束或取消。完成处理抛异常时显式 abort，不能追加第二份响应。

cancel 幂等释放正文、尝试 cancelBeforeStart 并取消 sender。AlreadyRunning 不会中断事务，AlreadyTerminal 不代表任务容量已归还。HTTP 状态释放后仍消费迟到结果；DirectoryTaskObserver 接收 request ID、task ID、可选 attempt、取消／错误分类及交付时的 HTTP 活动状态。观测异常被隔离，不改变业务结果或阻止响应。

RequestTrace 使用 exchange 编号通过注入的 RequestIdentity 获得服务端请求 ID，忽略客户端同名头，无全局交换表。记录不包含请求目标、query、凭据或正文。committed 和 finished 分别反映实际 HTTP 提交和终态，不证明客户端收到响应；响应头策略失败的底层兜底也不保证关联头。所有 sink 必须非阻塞且有界，模块不提供日志队列或事务审计。

muduox 在完整收体后等待结果时暂停读取，因此远端 FIN 不保证立即可见。取消来自实际已观察的断开、超时或停止等事件；断连不证明未运行或未提交。body 超时当前为 408，等待结果的 response 超时为框架 500，不是 504；定时器不能中断外部 worker 的阻塞操作。

## 上传与存储管理

TransferDispatcher 是 HttpApi 在 IdentityDispatcher 后的必填依赖；同时要求上传与下载能力，没有可选或兼容构造。九个上传／存储操作均传入自有 Bearer RequestContext。HTTP 仅做结构检查，资源权限、去重、剩余长度、预留和提交由应用负责。

创建仅接纳 client_upload_id、space_id、parent_id、name、length 及可选 sha256。ID 为非 nil 小写规范 UUID；length 为规范十进制字符串，范围 0～1 TiB；摘要为 64 位小写 hex。complete 必须 `{}` 或同时包含 parent_id/name。storage PUT 接纳 content_budget_bytes 规范十进制字符串及唯一强标签 `"storage-<instance>-<revision>"`，保留合法的异实例绑定交应用授权后判断。列表默认 100、最大 200，uploads 游标绑定用户／类型／created_at/id；client_upload_id 精确筛选与 cursor 互斥，不支持 state 筛选。DTO 固定输出可空字段，终态时间、结果节点、已校验摘要及状态枚举有表示校验；磁盘采样失败三字段全 null，revision 仅在 storage ETag。

PATCH 要求单一 Content-Length、Upload-Offset 和无参数 application/offset+octet-stream，仅缺省／identity 编码；拒绝 TE、trailer 和 multipart。缺长度 411、零长度 400、超过 8 MiB 413、媒体不支持 415；CL/TE 歧义由解析器 400 拒绝。协议层已知长度超路由限额时可由框架返回无 JSON 的 413。成功 HEAD 不输出表示长度，错误 HEAD 保留对应 JSON 的 Content-Type／Content-Length 但不发送正文。DELETE／PATCH 的 204 无正文／Content-Type。

PATCH 状态先异步 prepare，未获准时 body 返回 0。准备完成才 resumeBody；库的 100 Continue 不代表授权。只分配一个 64 KiB TransferBlock，feed 接纳移走、拒绝保留；至多一个在途块，回调取回原缓冲后才恢复输入。完整输入且所有 feed 退休后只提交一次 finish，确认成功才发送 204 及确认偏移。新请求检查 ready，已接纳 PATCH 在 HTTP grace 内继续 feed/finish。超时、断连和停止取消句柄与流；迟到 PreparedPatch 仍销毁／取消流，不触发逻辑 DELETE。

控制 JSON 复用公共异步状态及有界 sender；PATCH 的小响应独立处理 start/write/finish、acceptedBytes、部分接受和 WouldBlock。拒绝未消费输入时，发送完错误后显式 abort 输入，框架排空已提交输出并关闭连接，不能让尾部成为下一请求。仅 PatchOffsetConflict 成功变体暴露 details.offset；通用错误 details 为空。新上传冲突为 409、终止为 410、记录限额为 429；上传 StorageFull 为 503，原 mkdir StorageFull 保持 507，逻辑 QuotaExceeded 保持 403。未知提交不重试、不声称回滚。

[上传夹具](tests/UploadTestSupport.h) 用延后完成 gate 验证 pause/resume、同一缓冲归还、延迟 finish、prepare/feed/finish 阶段取消及 grace。测试不做磁盘 IO；小响应不证明真实 writer WouldBlock、文件同步／SQLite 持久性或完整 runtime 关闭。暂停读时 FIN 可能暂不可见，断连测试保留 HTTP 期限兜底。

## 内容下载

GET／HEAD 内容入口拒绝 query、正文和 Cookie，沿用 Bearer、自有请求 ID 与全局在途上限。只复制有界 If-Match、If-None-Match、Range、If-Range 头，异步提交 OpenDownloadCommand；HEAD 的 metadataOnly=true，GET=false。授权及 open/type/length 校验由应用／runtime 完成；成功结果之前不求值条件、不暴露摘要或长度。HEAD 成功描述不含流，GET 必须含流。

[DownloadResponse](DownloadResponse.h) 使用 muduox HttpConditional／HttpRange／HttpEntityTag 公共 API，先普通条件再 Range。强 ETag 为 `"sha256-<小写摘要>"`，没有 Last-Modified，忽略日期条件。被求值的非法 ETag 条件为 400、If-Match 失败 412、If-None-Match 命中 304；GET 单范围 206、有效不可满足范围 416，非法／多范围／未知单位和空文件 Range 忽略。HEAD 忽略 Range，If-Range 不匹配回退完整 200。

200／206 发送精确 Content-Length、application/octet-stream、Accept-Ranges、ETag、private,no-store、nosniff 和 attachment；ASCII filename 固定为 download，filename* 按 UTF-8 字节 percent 编码。HEAD 保留完整 GET 表示头且无正文。304 无 Content-Length／Content-Type／Content-Disposition；416 无正文、Content-Length 为 0，并保留 Content-Range 与 ETag。其余错误沿用 JSON envelope；HEAD 保留错误表示类型／长度并抑制正文。响应开始后的读取或发送错误只 abort，不追加 JSON。

[DownloadStream](DownloadStream.h) 在 writer owner loop 分别处理 start/write/finish 状态，延后 pull 不阻塞 loop。每交换只分配一个 64 KiB OwnedTransferBlock，至多一个在途读；回调归还同一块，writer 完全接纳后才读下一块。按每次实际 acceptedBytes 推进偏移并调用 consumed，含 WouldBlock 的部分接纳；零字节不报告进展。运行时取消回调仅弱引用 HTTP 状态，可在安装时或 consumed 内同步触发；局部流所有权保护重入，终止时清除回调并取消流。304／412／416、空文件、错误、超时、断开和停止均退休 GET 流；迟到 open 的流仍取消，迟到读不再发送或触发新读。HTTP 不自行释放文件 pin 或运行时槽。

内容路由 body limit 为 0，response 默认 21600 秒、write 3600 秒；HttpApiOptions.contentResponseSeconds 只允许有限正值收紧。JSON 的 30 秒 response 不限制内容下载；headers 15、idle 60、close 5 秒不变。新请求检查 ready，已接纳下载可在 HTTP grace 内完成。运行时无进展／禁用／停止通过同一取消回调终止等待 writable 的发送；已被 writer 接纳的字节不能撤回。

## 文件管理、搜索与回收站

DriveDispatcher 是 HttpApi 在 TransferDispatcher 后、observation/options 前的必填引用，无兼容构造或降级端口。八个入口复用 makeIdentityStream 和 AsyncRequestState，保持 128 HTTP 在途默认上限、JSON 请求 64 KiB、响应 512 KiB、正文 15 秒／响应 30 秒及 owner-loop writer 规则。HTTP 只提交自有命令，不执行 SQL、树权限判断、磁盘回收或 pin 管理。运行写入遇到断连／超时不等于回滚，迟到结果只消费，不发送第二份响应。

五个写入口接受唯一强 If-Match：`"node-<id>-<revision>"` 或 `"trash-<id>-<revision>"`，缺失 428，弱标签／列表／通配符／畸形输入 400。解析按标签自身种类构造 DriveCondition，保留 ID 与 revision；合法异类、异 ID 或陈旧标签必须交应用在授权／可见性及必要的树修改权限检查后判定。HTTP 不提前返回 412。update 至少含 parent_id/name 之一；copy 必需 space_id/parent_id/name；restore 的 `{}` 合法，两字段均可独立覆盖。重复／未知键、null、嵌套容器均拒绝。输入 ID 复用规范 UUID 解码，输出为规范小写。

搜索必需 space_id/q；q 复用 SearchText 领域校验，为 1～128 UTF-8 字节，无 NUL/C0/C1，不 trim 或改大小写，保留字面 `%`、`_`、反斜杠与空格。回收站列表必需 space_id，可选 root_node_id。两者 limit 默认 100、最大 200；其他操作拒绝 query，bodyless 操作拒绝非空正文。分页游标严格无填充 base64url，最多 2048 字节，解码 JSON 最多 1536 字节，含格式版本、排序版本及最后键。search 绑定 user/space/q 和 name/id；trash 绑定 user/space/可选 root_node_id 和 deleted_at/id。解析保留绑定值，应用负责授权后验证；游标不赋予权限，不承诺跨页快照。

TrashEntry 只序列化 id、instance_id、space_id、root_node_id、original_parent_id、name、kind、deleted_at、expires_at、revision、etag、node_count、logical_bytes。原父 ID 是非空历史信息，不承诺当前可访问；内部删除者、sole creator、成员和 Blob 不公开。时间为六位微秒 UTC RFC3339，数量／revision 为十进制字符串。列表每项含 etag，整页不设置单资源 ETag；永久删除 204 无正文。无操作 update 返回应用提供的原 revision；HTTP 不自行推进版本。

新增 CrossSpaceMove／DirectoryCycle／TreeLimitExceeded／NodeLimitReached／RestoreTargetUnavailable 映射对应 409；QueryLimitExceeded 映射 503 QUERY_LIMIT_EXCEEDED。注册、mkdir 和创建上传也沿公共映射返回 409 NODE_LIMIT_REACHED。文件管理元数据 StorageFull 为 507；上传 StorageFull 仍为 503，额度 PUT 的专用映射保留。队列／停止／GC 资格冲突为 503 TEMPORARILY_UNAVAILABLE；COMMIT_OUTCOME_UNKNOWN 保留 500，不声称回滚或自动重试，错误 details 保持空对象。

上传 JSON 必有 nullable result_deleted_at，取自 UploadView.resultDeletedAt，仅 COMPLETED 可非空且不早于 terminal_at；保留历史 result_node_id，入站本身不产生永久删除证据。所有上传 JSON 共用转换，HEAD 上传头不增加字段。GET／PUT storage 从实际 StorageView.gc／nodes 输出 pending_gc_bytes、pending_gc_objects、deleting_bytes、deleting_objects、used_nodes、reserved_nodes、node_limit 字符串。node_limit 取领域常量 100000；deleting 是 pending 的子集，pending 字节是 retained 的子集，不叠加计费。storage ETag 仍按原预算账本 revision，不绑定派生计数。

本地测试使用类型化假 dispatcher，验证原样传递条件和应用返回的 401／404／403／412，不替代真实授权事务、跨用户隔离、保留期／GC、恢复或服务装配验收。

## 构建与验证

[CMakeLists.txt](CMakeLists.txt) 定义静态库；[tests/CMakeLists.txt](tests/CMakeLists.txt) 在 ANOF_BUILD_TESTS 开启时注册十六个 Boost.Test 可执行文件，每个 CTest 超时为 30 秒，并为二十个公开头生成独立编译检查。

| 目标与源码 | 验证范围 |
|---|---|
| `anof_http_drive_protocol_unittest`：[DriveProtocol_unittest.cc](tests/DriveProtocol_unittest.cc) | Node／Trash 条件种类、严格 JSON／查询／游标、Trash DTO、上传删除证据、GC／节点统计和错误映射 |
| `anof_http_drive_handlers_unittest`：[DriveHandlers_unittest.cc](tests/DriveHandlers_unittest.cc) | 八个类型化入口、异类条件交应用、成功头／204、过载／停止／迟到／超时、共享容量和 keep-alive |
| `anof_http_download_protocol_unittest`：[DownloadProtocol_unittest.cc](tests/DownloadProtocol_unittest.cc) | 条件优先级、重复头、Range／If-Range、Unicode 附件名、HEAD／304／416、合成 1 TiB 长度 |
| `anof_http_download_handlers_unittest`：[DownloadHandlers_unittest.cc](tests/DownloadHandlers_unittest.cc) | GET／HEAD、授权前隐藏元信息、单块所有权与 consumed、迟到／取消／停止、真实 writer 背压恢复与取消 |
| `anof_http_transfer_protocol_unittest`：[TransferProtocol_unittest.cc](tests/TransferProtocol_unittest.cc) | 上传／storage JSON、条件、游标、DTO／头与新增错误分类 |
| `anof_http_upload_handlers_unittest`：[UploadHandlers_unittest.cc](tests/UploadHandlers_unittest.cc) | 九个入口、HEAD、PATCH pause/resume、单块所有权、迟到／取消／停止、错误关闭及 keep-alive |
| `anof_http_account_writes_unittest`：[AccountWrites_unittest.cc](tests/AccountWrites_unittest.cc) | 五个写入口的条件／命令／可信来源、DTO／204、错误映射、收体／任务容量、强制与优雅停止、提交异常恢复及 keep-alive 隔离 |
| `anof_http_account_reads_unittest`：[AccountReads_unittest.cc](tests/AccountReads_unittest.cc) | 四个账户管理读取入口、上下文／scope、用户游标、ETag、凭据／输入／方法、应用错误、容量及超时隔离 |
| `anof_http_account_protocol_unittest`：[AccountProtocol_unittest.cc](tests/AccountProtocol_unittest.cc) | 三类 If-Match、绑定保留、账户字段／嵌套额度边界、用户分页、账户 DTO／ETag 及操作错误映射 |
| `anof_http_identity_handlers_unittest`：[IdentityHandlers_unittest.cc](tests/IdentityHandlers_unittest.cc) | 六个身份／邀请操作的真实 loopback 请求、可信来源、匿名隔离、秘密成功响应、409／429／503、方法／输入拒绝及阶段取消 |
| `anof_http_reads_unittest`：[ReadHandlers_unittest.cc](tests/ReadHandlers_unittest.cc) | 四条读取路由的真实 loopback 请求、凭据／参数、DTO／游标、方法／期限／容量及迟到结果隔离 |
| `anof_http_identity_stream_unittest`：[IdentityStream_unittest.cc](tests/IdentityStream_unittest.cc) | 公共桥接的 loopback 交换、分块响应、取消／迟到、异常、秘密缓冲和任务夹具语义 |
| `anof_http_identity_protocol_unittest`：[IdentityProtocol_unittest.cc](tests/IdentityProtocol_unittest.cc) | 身份请求与 peer 校验、游标／查询／时间边界、DTO、秘密隔离、分页大小及新增错误映射 |
| `anof_http_protocol_unittest`：[Protocol_unittest.cc](tests/Protocol_unittest.cc) | 凭据／内容头、JSON 结构、Unicode、字节和时间边界、完整成功对象、错误映射及 muduox 序列化 |
| `anof_http_api_unittest`：[HttpApi_unittest.cc](tests/HttpApi_unittest.cc) | 真 HTTP 健康检查、404/405、请求 ID、头部拒绝、路由限额、容量归还、keep-alive 和框架错误 |
| `anof_http_create_directory_unittest`：[CreateDirectoryHandler_unittest.cc](tests/CreateDirectoryHandler_unittest.cc) | 真 HTTP 加可控 dispatcher，验证分片/chunked/Expect/trailer、一次提交、延后结果、201/401/503/507、取消、超时、迟到结果、停止和故障注入 |
| `anof_http_public_headers_check` | 二十个头文件在独立翻译单元中编译，检查 include 自足性 |

从仓库根目录复用现有 Unix Makefiles 构建目录，开启 ANOF_BUILD_TESTS，保持 MUDUOX_BUILD_TESTS／MUDUOX_BUILD_EXAMPLES 为 OFF。以下为 Debug 命令；Release 使用 `cmake-build-release` 和 `-DCMAKE_BUILD_TYPE=Release`，两组顺序执行，串行运行 CTest：

```sh
cmake -S . -B cmake-build-debug -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DANOF_BUILD_TESTS=ON -DMUDUOX_BUILD_TESTS=OFF -DMUDUOX_BUILD_EXAMPLES=OFF
cmake --build cmake-build-debug --target anof_http_drive_protocol_unittest anof_http_drive_handlers_unittest anof_http_download_protocol_unittest anof_http_download_handlers_unittest anof_http_transfer_protocol_unittest anof_http_upload_handlers_unittest anof_http_account_writes_unittest anof_http_account_reads_unittest anof_http_account_protocol_unittest anof_http_identity_handlers_unittest anof_http_reads_unittest anof_http_identity_stream_unittest anof_http_identity_protocol_unittest anof_http_protocol_unittest anof_http_api_unittest anof_http_create_directory_unittest anof_http_public_headers_check --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_http_(drive_protocol|drive_handlers|download_protocol|download_handlers|transfer_protocol|upload_handlers|account_writes|account_reads|account_protocol|identity_handlers|reads|create_directory|api|protocol|identity_protocol|identity_stream)_unittest$' -j 1
```

网络测试使用 127.0.0.1 端口 0 并读取实际监听地址，使用小 JSON、显式完成 gate 和有限 timer，不使用固定 sleep。等待统一复用 `anof::test::waitFor`，保持 3 秒上限和 1 毫秒轮询，转发实际调用位置并由共享工具处理 `ANOF_WAIT_TRACE`。首个观察到的完成／超时终态固定结果，未观察到完成的外部 quit 返回失败；返回和诊断不重新执行可能消费 socket 数据的谓词，谓词异常在退出 loop、清理定时器后重抛。该规则表示回调观察顺序，不承诺物理完成时刻的硬实时界限。它们覆盖 Queued、Running、AlreadyTerminal、重复／优雅停止、超时后 CommitOutcomeUnknown、ready/提交/观测/响应头异常。writer 弱引用过期约束请求状态不得被迟到回调强保活，但不等同于一般内存泄漏检测。提交异常只模拟接纳前失败，不支持“接纳后抛出且不给句柄”的违约 dispatcher。

生命周期回归还覆盖：优雅停止期间已接纳任务正常完成、重复 stop 不替换通知；结果已排队时取消返回 AlreadyTerminal 后不序列化秘密；错误 task ID 终止当前交换，重复旧 completion 不影响下一次 keep-alive 请求。后两项属于 dispatcher 违约故障注入，不改变端口“延后恰一次完成”的契约。真实路由测试验证收体期间关闭 ready、提交前异常后的容量恢复、秘密结果类别错误，以及 HTTP 名额归还后密码任务仍占用其独立名额直到完成交付。账户写入口还验证改密收体中断／超时及 ready 关闭均不提交、五种写操作提交异常后名额恢复、改密空 204 后同连接额度错误的请求 ID 与响应隔离；强制停止在 Queued／Password／FinalTransaction 阶段完成 HTTP 清理后仍可交付迟到结果，优雅停止允许已接纳改密完成且仅通知一次。停止测试只证明注入任务与 HTTP 的局部生命周期，不证明实际 worker 退出屏障。

普通 JSON／上传响应未验证真实 writer WouldBlock。下载组件测试通过公共 setOutputLimits 设置 64 KiB 高水位，以至多 8 MiB 合成内容和 loopback 暂停读取，观察真实 write WouldBlock、部分 acceptedBytes、同一块的未消费余量、恢复发送及等待 writable 时取消；只读取发送状态已有字段，不添加生产指标或伪造 writer 状态。start WouldBlock 未观察到；固定 Content-Length 的 finish 不发送 chunked 终结帧，未触发 finish WouldBlock。测试使用注入读取流，不验证真实文件／SQLite 与 writer 的组合背压、crypto、完整 runtime 双侧关闭屏障或物理机持久性。
