# HTTP 协议参考

本文用于实现调用方或修改协议转换。一次请求先经过[安全入口](SECURITY.md)，再由路由选择具体 parser 和 dispatcher；下表描述正常通过预检后的操作。应用负责身份、可见性和事务，transport 负责字段格式及线上表示。异步发送与取消见[生命周期说明](STREAMS.md)。

## 路由

下表列出 41 个方法／路径组合，花括号表示路径参数。除健康、注册和登录外均要求业务凭据。安全检查和容量限制先于路由分派，因此错误来源请求不保证得到 404；通过这些检查后，未知路径为 404，已知路径的方法不匹配为 405 并带 Allow。只有表中显式 HEAD 可用，不自动回退 GET，也不自动处理 OPTIONS。

| 端点 | 成功行为或返回表示 |
| --- | --- |
| `GET /health/live` | 返回 `{"status":"live"}`，不访问业务存储 |
| `GET /health/ready` | ready=true 时返回 `{"status":"ready"}`，否则为 503 错误 envelope |
| `POST /api/v1/auth/register` | 匿名注册，201 用户资料，不创建 Session |
| `POST /api/v1/auth/sessions` | 匿名登录，201 按模式返回 Bearer token 或 Set-Cookie、到期时间及用户资料 |
| `DELETE /api/v1/auth/session` | 登出当前 Session，成功 204 |
| `POST /api/v1/admin/invites` | 创建邀请，201 元信息及一次性明文 token |
| `GET /api/v1/admin/invites` | 邀请元信息分页，包含已撤销/失效项 |
| `DELETE /api/v1/admin/invites/{invite_id}` | 撤销邀请，成功或已撤销为 204 |
| `GET /api/v1/users/me` | 当前用户资料、instance_id 及原用户 ETag |
| `PATCH /api/v1/users/me` | 修改本人显示名，200 用户资料及 ETag |
| `POST /api/v1/users/me/password` | 改密成功 204，无新 token/ETag |
| `PATCH /api/v1/admin/users/{user_id}` | 启禁用户，200 管理用户详情及 ETag |
| `GET /api/v1/admin/users` | 管理用户分页，无集合 ETag |
| `GET /api/v1/admin/users/{user_id}` | 管理用户详情与私有空间 ID，带返回用户 ETag |
| `PUT /api/v1/admin/spaces/{space_id}/quota` | 更新空间额度 limit，200 管理额度及 ETag |
| `PUT /api/v1/admin/spaces/{space_id}/contributions/{user_id}/quota` | 更新贡献额度 limit，200 管理额度及 ETag |
| `GET /api/v1/admin/spaces/{space_id}/quota` | 空间额度，user_id 为 null，带额度 ETag |
| `GET /api/v1/admin/spaces/{space_id}/contributions/{user_id}/quota` | 用户贡献额度，带绑定空间和用户的额度 ETag |
| `GET /api/v1/spaces` | 本人私有空间和公共空间及独立额度 |
| `POST /api/v1/nodes` | 创建目录，201 目录信息＋ETag＋Location |
| `GET /api/v1/nodes/{node_id}` | 活动可见节点元数据，头部/正文 ETag 一致 |
| `GET /api/v1/nodes/{node_id}/children` | 目录直接子项分页及绑定游标 |
| `PATCH /api/v1/nodes/{node_id}` | 改名/移动或组合更新，200 NodeMetadata＋ETag |
| `POST /api/v1/nodes/{node_id}/copy` | 单文件复制，201 NodeMetadata＋ETag＋node Location |
| `DELETE /api/v1/nodes/{node_id}` | 移入回收站，200 TrashEntry＋ETag＋trash Location |
| `GET /api/v1/search` | 按空间和字面名称查询一页活动节点 |
| `GET /api/v1/trash` | 按空间、可选原根 ID 查询一页回收站条目 |
| `GET /api/v1/trash/{trash_id}` | 条目详情，200 TrashEntry＋ETag |
| `POST /api/v1/trash/{trash_id}/restore` | 恢复，200 NodeMetadata＋ETag＋node Location |
| `DELETE /api/v1/trash/{trash_id}` | 永久删除确认，204；不表示物理回收完成 |
| `POST /api/v1/uploads` | 创建/重放上传，按返回 disposition 为 201/200/202 |
| `GET /api/v1/uploads` | 上传分页或 client_upload_id 精确筛选 |
| `GET /api/v1/uploads/{upload_id}` | 查询自有上传及保留终态 |
| `HEAD /api/v1/uploads/{upload_id}` | 200 仅上传头，无正文/Content-Type/Content-Length |
| `PATCH /api/v1/uploads/{upload_id}` | 1 字节～8 MiB 原始字节，finish 确认后 204＋Upload-Offset |
| `DELETE /api/v1/uploads/{upload_id}` | 逻辑取消确认 204，不代表磁盘清理完成 |
| `POST /api/v1/uploads/{upload_id}/complete` | 接纳后台校验/发布 202，已完成 200 |
| `GET /api/v1/nodes/{node_id}/content` | 授权 open 后发送完整内容/单范围，支持条件请求 |
| `HEAD /api/v1/nodes/{node_id}/content` | metadataOnly open，返回完整表示头，无正文、无流式槽 |
| `GET /api/v1/admin/storage` | 预算、计量、独立 ETag 与磁盘采样 |
| `PUT /api/v1/admin/storage` | 强 If-Match 条件更新预算，200 |

健康路由正文限额为 0，不调用凭据摘要或业务存储，但仍经过来源检查和全局容量限制。其余业务请求在未就绪时返回 503。`HEAD` 上传详情的成功头为 Upload-Offset、Upload-Length、Upload-State、Upload-Expires；错误 HEAD 保留错误表示的类型与长度，但不发正文。

## 公共输入和表示

JSON 请求的媒体类型为 `application/json`，允许一个 UTF-8 charset 参数；Content-Encoding 只能缺省或为 identity。重复歧义内容头返回 400，不支持的媒体类型／编码返回 415。JSON 正文最多 65536 字节，未知字段、解码后重复键、错误类型、尾随 JSON 值和非法 Unicode 都被拒绝。解析使用 SAX，即逐项消费 JSON 事件；平坦命令不接受嵌套容器，额度更新使用专用两层解析器。

无正文业务请求在头部检查中拒绝非零 Content-Length、Transfer-Encoding 和 trailer，并在完整输入时再次检查。已知正文长度超路由限额可由框架更早返回非 JSON 的 413。创建目录和健康入口当前未单独校验 query；调用方应只使用表中的路径。其余不提供查询功能的业务入口拒绝查询参数，列表和搜索只接受下文列出的字段。

输出 ID 使用规范小写 UUID；涉及 64 位数量和 revision 的字段通常使用规范十进制字符串，避免客户端丢失整数精度。邀请次数和上传的 `max_patch_bytes` 是 JSON 整数。时间输出为 UTC 六位微秒，支持公历 0001～9999 年；时间解析不接受省略小数、其他时区或无效日期。输入 ID 的具体严格度由各 parser 决定，条件标签、上传 ID 和游标要求非 nil、规范小写值。

模块生成的 JSON 带 `Content-Type: application/json`、`Cache-Control: no-store` 和服务端 `X-Request-ID`；401 另带 Bearer challenge。普通错误形状如下，ID 仅为格式示例：

```json
{"error":{"code":"INVALID_ARGUMENT","message":"Invalid request","request_id":"40000000-0000-4000-8000-000000000001","details":{}}}
```

错误不回显秘密或当前资源值。只有授权后的 PATCH 偏移冲突专用结果会在 `details` 中给出 `offset`。框架错误、HEAD 与连接中止不保证 JSON；客户端不能无条件解析正文。204 无正文和 Content-Type，仍保留缓存与请求关联头。

分页和身份类 JSON 响应整体最多 512 KiB，不受请求的 64 KiB 上限约束；发送时再分块。`JsonResponse` 将正文与头分开保存，不自行指定 Content-Length 或 Transfer-Encoding，由缓冲响应或流式发送器处理定界。转换器校验结果形状失败只表示不能生成正常响应，不能据此推断应用事务已回滚。

## 身份、账户和条件写入

[IdentityRequest](IdentityRequest.cc) 解析身份命令，[AccountRequest](AccountRequest.cc) 解析账户修改。字段含义和类型如下：

| 操作 | 正文字段 |
| --- | --- |
| 注册 | 必填 login、display_name、password、invite_token；可选 credential_mode，默认 bearer |
| 登录 | 必填 login、password、credential_mode，模式为 bearer 或 cookie |
| 创建邀请 | max_uses 可省略，默认 1、范围 1～10000；valid_for_seconds 默认 604800、范围 1～2592000；空对象合法，空正文不合法 |
| 本人改名 | 仅 display_name 字符串 |
| 本人改密 | current_password 和 new_password 字符串 |
| 管理员启禁用户 | 仅 enabled 布尔值 |
| 更新额度 | limit 对象，必需 bytes、nodes 两个规范十进制字符串，范围 0～INT64_MAX |

登录名按领域规则转 ASCII 小写。密码不 trim 或归一化；显示名为 1～128 UTF-8 字节，密码输入统一要求 12～1024 UTF-8 字节，由领域输入类型校验。匿名请求的旧 Cookie、模式选择和秘密交付规则集中在[安全说明](SECURITY.md#登录与注册)。

五个账户写操作都要求唯一强 If-Match，支持以下三种标签形状：

```text
"user-<user_id>-<revision>"
"quota-space-<space_id>-<revision>"
"quota-contribution-<space_id>-<user_id>-<revision>"
```

只去除头值外围空格／制表符，revision 为 1～INT64_MAX 的规范十进制。缺失返回 428，重复、弱标签、列表、通配符、空值或畸形标签返回 400。合法但种类、ID 或版本不匹配的标签仍传给应用，应用先鉴权再判定 412，HTTP 不提前比较当前资源。登录、注册、邀请创建和创建目录不增加这一条件要求。

[IdentityResponse](IdentityResponse.cc) 的 UserProfile 包含 id、login、display_name、role、enabled、revision、created_at、updated_at，不暴露 auth epoch、密码版本或摘要。只有 GET me 的 CurrentUserView 额外输出 instance_id，保持原用户 ETag；登录、注册和改名的用户形状不因此改变。管理用户形状为 `{user,private_space_id}`，管理额度形状为 `{space_id,user_id,quota}`，空间额度的 user_id 为 null。

用户和额度 ETag 来自返回资源的 revision；列表不生成单资源 ETag。元数据 JSON GET 不实现条件缓存，If-None-Match 不使其返回 304。改密成功为 204，不返回新 token 或 ETag。旧密码不正确、最终 Session 失效和条件变化分别由应用返回 403、401、412。

## 节点与文件管理

创建目录要求 space_id、parent_id、name 三个字符串。名称使用领域规则，范围 1～255 UTF-8 字节，不 trim、折叠大小写或归一化。成功表示含 id、space_id、parent_id、name、kind、created_by、created_at、updated_at，Location 指向新节点，初始 ETag 为 `"node-<id>-1"`。转换器要求有父 ID、revision 为 1 且创建／修改时间一致。

节点读取表示区分空名、无父节点的根目录，以及带 size／sha256 的文件；节点 GET 的正文 etag 与头部一致。空间列表返回按 ID 排序的私有与公共空间，并校验 owner 和贡献额度的表示形状；可见性仍由应用判断。

[DriveHandlers](DriveHandlers.cc) 的五个写入口要求唯一强 If-Match，可解析 `"node-<id>-<revision>"` 和 `"trash-<id>-<revision>"`。格式错误与缺失沿用 400／428，合法异类、异 ID 或陈旧标签交应用在授权及树权限检查后判断，不能提前泄露目标状态。

| 操作 | JSON 约束 |
| --- | --- |
| 改名／移动 | 至少提供 parent_id、name 之一 |
| 单文件复制 | 必需 space_id、parent_id、name |
| 恢复回收站条目 | `{}` 合法；parent_id、name 可独立覆盖 |

这些对象拒绝未知／重复键、null 和额外嵌套。移动入回收站与永久删除不接受正文。无实际变动的修改使用应用返回的原 revision，HTTP 不自行递增版本。

TrashEntry 输出 id、instance_id、space_id、root_node_id、original_parent_id、name、kind、deleted_at、expires_at、revision、etag、node_count、logical_bytes。原父 ID 是历史信息，不保证该父节点仍可访问。内部删除者、成员清单和 Blob 不公开。列表每项带 etag，整页不设单资源 ETag；永久删除 204 表示逻辑操作确认，不表示物理文件已回收。

## 列表、搜索与游标

分页默认 limit=100，范围 1～200；拒绝重复参数、空值、前导零、符号、小数和指数。响应为 items 与 next_cursor，末页为 null，空页不能带下一页游标。编码前检查下一页位置与最后一项匹配；排序、唯一性及权限由应用负责。

| 列表 | 允许的查询与绑定 |
| --- | --- |
| children | limit、cursor；游标绑定用户、空间、父目录及 name／id 位置 |
| 邀请 | limit、cursor；绑定用户及 created_at／id |
| 管理用户 | limit、cursor；绑定管理员用户及 after_id |
| 上传 | limit、cursor 或 client_upload_id 精确筛选；精确筛选与 cursor 互斥，不支持 state 筛选 |
| 搜索 | 必需 space_id、q，可选 limit、cursor；绑定用户、空间、搜索文本及 name／id |
| 回收站 | 必需 space_id，可选 root_node_id、limit、cursor；绑定用户、空间、可选原根及 deleted_at／id |

搜索 q 是 1～128 UTF-8 字节，不允许 NUL/C0/C1 控制字符，不 trim 或改大小写；`%`、`_`、反斜杠和空格保留字面意义。查询使用百分号解码，不能把搜索文本当 SQL 模式。

游标为无 padding 的规范 base64url，最多 2048 字符，解码 JSON 最多 1536 字节。解析检查形状、版本、ID、名称和时间；搜索／回收站还保留排序版本。transport 只传递绑定值，应用在授权后核对。游标不授予权限，也不承诺跨页快照。实现入口为 [Pagination](Pagination.h)、[DriveRequest](DriveRequest.cc) 和 [TransferRequest](TransferRequest.cc)。

## 上传与存储

创建上传要求 client_upload_id、space_id、parent_id、name、length，可选 sha256。length 是 0～1 TiB 的规范十进制字符串，sha256 为 64 位小写十六进制；ID 要求非 nil、规范小写 UUID。完成请求必须为 `{}`，或同时给出 parent_id 和 name。创建／重放的 201、200、202 由 dispatcher 的 disposition 决定；只有新建 201 设置上传 Location。完成操作的 202 表示已接纳校验／发布流程，不能据此认定文件已发布。

上传 JSON 固定输出可空字段。offset 不超过 length，终态有 terminal_at，COMPLETED 有 result_node_id，READY／COMMITTING／COMPLETED 有 actual_sha256。result_deleted_at 只允许在 COMPLETED 非空且不早于 terminal_at；即使结果已删除，历史 result_node_id 仍保留。HTTP 不自行产生删除证据或推进上传状态。状态和错误枚举由 [TransferResponse](TransferResponse.cc) 映射。

PATCH 正文是原始字节，大小 **1 字节～8 MiB**，要求唯一 Content-Length、Upload-Offset 和无参数 `application/offset+octet-stream`，编码只能缺省或 identity。缺长度为 411、零长度为 400、超限为 413、不支持的媒体为 415；拒绝 Transfer-Encoding、trailer、multipart 和 CL/TE 歧义。成功必须等 finish 确认后才返回 204 与确认偏移，输入被 HTTP 消费不代表已持久化。数据块与取消过程见[上传生命周期](STREAMS.md#上传如何暂停输入)。

Storage PUT 要求 content_budget_bytes 规范十进制字符串及唯一强标签 `"storage-<instance_id>-<revision>"`；合法异实例绑定交应用授权后判断。GET／PUT 表示含预算、retained／reserved、磁盘采样，以及以下十进制字符串：

- pending_gc_bytes／objects、deleting_bytes／objects；deleting 是 pending 的子集。
- quarantined_bytes／objects、unavailable_bytes／objects；与 pending 分类互斥，均属于 retained，不能重复加算为额外使用量或可用空间。
- used_nodes、reserved_nodes、node_limit；节点上限取领域常量 100000。

序列化先校验计量子集和受检求和，但不会仅因异常占用使 retained＋reserved 超过预算就拒绝只读响应。磁盘采样不可用时 free_bytes、safety_margin_bytes、sampled_at 全为 null。storage ETag 取预算账本 revision，不绑定这些派生计数，revision 不另放入正文。

## 内容下载与条件请求

GET／HEAD 内容入口拒绝 query 和正文，只复制 If-Match、If-None-Match、Range、If-Range，合计最多 100 个字段、65536 字节值。请求先提交 `OpenDownloadCommand`，由应用／runtime 完成授权和文件类型／长度检查，成功后才计算条件；未授权者不能借条件响应探测摘要或长度。HEAD 的 metadataOnly=true，返回描述且不带读取流。

[DownloadResponse](DownloadResponse.cc) 使用 muduox 的条件和 Range 接口。ETag 为强标签 `"sha256-<小写摘要>"`；没有 Last-Modified，日期条件不据此生效。先普通条件再 Range：

| 求值结果 | 响应 |
| --- | --- |
| 被求值的 ETag 条件非法／If-Match 不满足 | 400／412 |
| If-None-Match 命中 | 304，无 Content-Length、Content-Type、Content-Disposition |
| GET 单范围可满足 | 206，精确长度及 Content-Range |
| 有效范围不可满足 | 416，长度 0，无正文，带 Content-Range 和 ETag |
| 非法／多范围／未知单位／空文件 Range | 忽略 Range，使用完整表示 |
| HEAD 的 Range／不匹配的 If-Range | HEAD 忽略 Range；If-Range 不匹配退回完整表示 |

200／206 带精确 Content-Length、application/octet-stream、Accept-Ranges、ETag、private,no-store、nosniff 和 attachment。ASCII filename 固定为 download；filename* 按原名 UTF-8 字节百分号编码。HEAD 保留表示头而不发正文。其他错误使用普通 JSON 表示，HEAD 仍保留其类型和长度。响应已开始后发生读取或发送错误只能中止，不能追加一段 JSON。

## 错误分类

错误描述集中在 [JsonResponse.cc](JsonResponse.cc)，操作专用转换保留不同语义。以下几类尤其不能合并处理：

| 情况 | 状态与 code |
| --- | --- |
| Session 上限 | 409 SESSION_LIMIT_REACHED |
| 来源／登录名速率限制 | 429 RATE_LIMITED，包括同步拒绝和已接纳任务的业务错误 |
| 队列满、停止接纳、可重试冲突 | 503 TEMPORARILY_UNAVAILABLE |
| 查询工作量限制 | 503 QUERY_LIMIT_EXCEEDED |
| 默认逻辑额度不足／管理员额度 PUT 下调不合法 | 403 QUOTA_EXCEEDED／409 QUOTA_EXCEEDED |
| 上传、storage 及下载路径的磁盘不足／默认元数据写入磁盘不足 | 503 STORAGE_FULL／507 STORAGE_FULL |
| 下载 open 或复制的内容完整性错误 | 500 INTEGRITY_ERROR；其他操作的通用 IntegrityError 为 500 INTERNAL_ERROR |
| 提交结果未知 | 500 COMMIT_OUTCOME_UNKNOWN；写入可能已提交，不自动重试 |
| 上传已终止／上传记录限额 | 410 UPLOAD_TERMINATED／429 UPLOAD_LIMIT_REACHED |

上传键、偏移、长度、状态和完成阶段冲突以及实例预算冲突使用对应 409 code。文件管理的 CROSS_SPACE_MOVE、DIRECTORY_CYCLE、TREE_LIMIT_EXCEEDED、NODE_LIMIT_REACHED、RESTORE_TARGET_UNAVAILABLE 和 NOT_A_FILE 也为 409。未认证为 401，不可见资源通常由应用返回 404；这些业务结论不能由 transport 的格式检查代替。
