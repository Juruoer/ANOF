# 请求、凭据与文件传输

`anof_client` 提供同步、类型化的远程调用和本地 profile 存储。依赖 `anof_domain`、`CURL::libcurl` 、nlohmann-json 和 libsodium（本地 SHA-256 及随机传输 ID），不链接服务端应用服务、SQLite、infrastructure、runtime 或 muduox。库不处理终端、进程信号或标准输出；调用者负责展示结果和设置取消标志。

## 从 profile 到一次请求

- [ProfileStore.h](ProfileStore.h)：解析配置目录，添加、枚举、查看、删除 profile，以及获取持锁的 `LockedProfile`。list/show 返回脱敏摘要。
- [ApiClient.h](ApiClient.h)：注册、登录、登出、当前用户、邀请创建/枚举/撤销、空间、目录枚举、节点详情和创建目录，以及改名/改密、管理用户分页/详情/启禁、空间和公共贡献额度读取/调整。只请求一页；UUID 与受检领域值表达输入，DTO 表达输出。
- [HttpTransport.h](HttpTransport.h)、[CurlHttpTransport.h](CurlHttpTransport.h)：传输接口、规范 origin 和 libcurl 实现，可注入传输替身测试协议与凭据处理。
- [Protocol.h](Protocol.h)、[ClientTypes.h](ClientTypes.h)：响应验证、类型化 DTO、固定容量秘密缓冲、错误及远端/本地结果分类。公共接口不暴露 curl 类型；传输 DTO 提供经逐字段验证、重建后的只读 JSON 视图。

调用者先获得 `LockedProfile`，再输入秘密并构造 `ApiClient`。transport、profile 和可选 `atomic_bool` 取消标志必须活到同步调用结束。一个持锁句柄用于一个命令；相同 profile 的并发命令立即失败，不等待锁。登录结果已剔除 access token；只有创建邀请的结果包含需交给用户的邀请码。

```cpp
auto held = store.lock("family");
if (!held) { /* 展示固定错误及退出码 */ }
else {
    anof::client::CurlHttpTransport transport;
    anof::client::ApiClient api(transport, held.value());
    auto result = api.me();
    // 使用类型化结果，不转发原始 HTTP 正文。
}
```

## 请求发送与响应检查

服务地址仅为 origin，拒绝 userinfo、非根路径、query、fragment 和控制字符。HTTPS 使用系统信任库并验证主机名；HTTP 仅允许显式开发模式的字面 `127.0.0.1`/`[::1]`。无逐请求地址覆盖、重定向、代理环境、netrc、Cookie、自动重试或自动登录。

连接期限默认 5 秒，总期限 35 秒；构造参数只能缩短期限。传输超限时中止读取，避免远端响应耗尽客户端内存。请求正文最多 64 KiB，响应正文 512 KiB，所有头区段累计 64 KiB。使用固定容量正文缓冲，拒绝重复关键头和 trailer；协议层只接纳 identity 编码。GET/DELETE 无正文，POST/PATCH/PUT 提供已知长度；Bearer 仅发送给受保护接口。C 回调捕获异常，不让异常跨越 C ABI。

响应检查状态、JSON 类型/深度/重复键、必需字段、规范 UUID/UTC/token、64 位十进制数量、ETag、分页和已知错误码映射。未知对象字段被忽略，不进入输出 DTO；远端错误消息不进入客户端诊断。`me()` 返回 `CurrentUser{user, instanceId}`，必须包含规范 instance_id；注册、登录、改名及管理员用户模型不变。三个同源安全错误仅接受 403 并归类为已知拒绝，CLI 仍不发送 Cookie。me/stat 的标签和节点内容必须一致，mkdir 还检查 Location。游标仅编码一次，不自动遍历下一页。账户响应验证资源与 ETag 一致，管理查询/写入还核对返回目标。调额的 QUOTA_EXCEEDED 接受 409，mkdir 仍接受 403。

账户写方法要求 [ConditionTag](ClientTypes.h)，从完整带双引号的用户/空间额度/贡献额度标签解析（同一类型也支持存储、节点及回收站标签）；拒绝弱标签、通配、列表、非规范 ID/版本及控制字符。不自动 GET 或刷新版本，合法错配绑定交给服务端授权后判定。`QuotaTarget` 的可选 userId 区分空间和公共贡献；`Amount` 保持受检 64 位数量。If-Match 是独立请求字段，不提供任意自定义头注入。

改密经 `changePassword` 收到合法 204 后才清除当前持锁 profile 的凭据；不更新其他 profile，不自动登录。明确拒绝、提交未知、传输失败或不合法成功响应均保留凭据，保留不代表其仍有效。清除失败返回本地错误 5，保留远端 confirmed_success 及本地 unchanged/unknown；调用方显式核对或重新登录，不重发改密。

## profile 如何保存

配置路径优先取显式绝对路径，其次非空 `$XDG_CONFIG_HOME/anof`，最后 `$HOME/.config/anof`。只在 add 时创建缺失目录；缺失目录上的 list 返回空列表。每个 profile 是 `profiles/NAME.json`，NAME 为 `[a-z][a-z0-9_-]{0,31}`；单文件最多 16 KiB，目录最多 100 个合规命名的 profile。文件 version 固定为 1，严格拒绝未知/重复字段。

实际配置目录及 profiles 目录须由有效 UID 拥有且权限 0700，数据和稳定锁文件须为同 UID 的 0600 普通单硬链接文件。以目录描述符和 `openat(O_NOFOLLOW)` 操作，拒绝路径中的符号链接及 `..`；已有不安全对象不自动修复。

目录锁协调 add/remove/list，profile 锁覆盖加载、HTTP 和保存，均为非阻塞排他锁。获取顺序为目录锁→profile 锁；远程命令只持 profile 锁。remove 保留稳定锁文件，仅删除本地配置，不撤销远端会话。

保存通过同目录临时文件、完整写入、文件 fsync、原子发布和目录 fsync 完成；add 使用 Linux `renameat2(RENAME_NOREPLACE)`，不会覆盖旧 profile。发布前失败保持旧数据，发布后同步失败标记本地结果未知，该句柄不可继续调用 API 或修改凭据，须释放后重新加载。 `SaveHook` 是验证发布边界的故障注入点。

凭据与 origin 同文件绑定，保存 token、到期时间、用户 ID 和 login，不保存密码/邀请码。凭据为 0600 明文；自有秘密缓冲在释放前清零，不承诺擦除第三方库、内核或介质副本，也不抵御同 UID/root 的主动篡改。

## 请求失败后如何判断结果

`ClientError` 提供固定类别、稳定 code、可信 HTTP 元数据、`remote`/`local` 状态及客户端退出码。配置内容损坏可附带已经验证的 profile 名称，不附带文件或响应原文。

| 退出码 | 含义                                                 |
|--------|------------------------------------------------------|
| 2      | 参数非法，尚未发请求                                 |
| 3      | 缺少本地凭据或合法 ANOF 拒绝                         |
| 4      | 读取的网络/协议失败，或传输开始前初始化失败          |
| 5      | 本地配置、锁、保存或内部处理失败                     |
| 6      | 写请求已开始但无法确认结果，或服务端明确返回提交未知 |
| 130    | 本地或读取操作中断                                   |

只有传输尚未开始时使用 `not_sent`；读取请求发出后失败仍用 4/130 退出码，但远端结果为 unknown。写请求在开始传输后发生超时、中断或响应损坏时保守视为未知，绝不自动重发。登录只有验证成功并保存凭据后才成功；落盘失败保持远端 `confirmed_success`。登出即使远端失败也尝试清除本地凭据，清除失败优先返回本地错误，并保留远端结果。其他 401 不清除凭据。上述退出码供调用者使用，不改变本地 init/serve 的既有语义。

## 上传和下载如何恢复

[TransferClient](TransferClient.h) 提供上传、下载、显式 resume 以及上传分页/查询/complete/cancel、storage 读取及带条件更新。 [TransferProtocol](TransferProtocol.h) 验证传输数量、状态、摘要、实例绑定、标签、偏移及已知错误码，剔除未知字段。 [TransferTransport](TransferTransport.h) 提供同步借用的有界 source/sink 及响应头验证回调；curl 复用 JSON 调用的 TLS、origin、代理与重定向规则。 HEAD 为只读。二进制 source/sink 每次至多 64KiB，不扩大 JSON 正文上限；PATCH 默认 8MiB，每次仅一个请求。 PATCH 总期限 365 秒，内容 GET 总期限 21660 秒，连接 5 秒，实际字节 120 秒无进展取消。TransferTimeouts 只能缩短期限。

调用者持有 profile 锁覆盖整个命令；传输记录另持稳定排他锁，目录锁只用于短时创建/计数。 配置根下 transfers 目录 0700、记录 0600，每条最多 16KiB、总数 1024；校验 UID、单硬链接、普通文件和 no-follow。 记录保存 profile/origin/user、已知 instance、源/目标、摘要、阶段及持久偏移，不保存凭据。 记录以文件同步、原子替换和目录同步发布，已完成记录保留。记录不会自动淘汰，需要调用者管理保留的记录。

### 上传

上传持有同一普通文件 FD，流式预哈希，核对 dev/ino/size/mtime/ctime；仍需传输时恢复重哈希，每 PATCH 前后复核；已完成/后台发布状态只查询，不依赖源文件仍然存在。 创建前保存随机 client_upload_id 及完整初始记录。未知创建结果只能按原键查询，未找到也不自动重发 POST。 恢复先 HEAD/GET，以服务端确认 offset 为准；未知 PATCH 不自动重试。complete 后每秒有界查询，最多 60 次；成功结果的 published 区分 COMPLETED 和已接纳，READY 错误须显式 complete 修正目标。

### 下载

下载先 HEAD 验证长度和强 sha256 ETag，再使用 If-Match 或 Range/If-Range。只在头验证通过后写 part，非零偏移收到 200 会拒绝追加。part 与目标同目录，路径分量不跟随符号链接，父目录须由当前 UID 控制且不可组/其他写入。 每 8MiB 或请求结束 fdatasync 并保存 durable_offset；恢复截断未确认尾部、拒绝短文件或 inode 替换。 完整长度/摘要验证后保存 PUBLISHING，以不覆盖 rename 发布、同步父目录并保存 COMPLETED。 发布结果不明时根据目标/part 身份和摘要确认，冲突不覆盖；part 已创建但身份记录尚未保存的歧义现场拒绝自动接管。 源文件允许普通用户文件权限，凭据/part 的 0600 要求不用于源文件。

### 本地与远端结果

传输错误包含可恢复的 transfer_id；本地状态增加 transfer_saved、download_partial、file_published。 远端确认成功但本地保存/输出失败仍保留 confirmed_success；未知本地发布用 unknown。 正常恢复不会自动删除服务端上传、覆盖既有目标或跳过摘要验证。

[传输测试](tests/TransferClient_unittest.cc) 覆盖未知创建/PATCH、持久偏移、200 替代 206、摘要失败、part 替换、 目标冲突、记录容量/权限/锁；[curl 测试](tests/CurlHttpTransport_unittest.cc) 验证真实二进制回调、头门禁及有限期限。 [CLI 进程测试](../../tests/integration/TransferClientProcess_unittest.cc) 覆盖真实上传、下载、跨进程续传及服务重启。 测试使用小文件与缩短的期限，验证的是恢复分支和错误处理。

## 文件管理请求

[DriveClient](DriveClient.h) 以类型化 NodeId/SpaceId/TrashId 和显式 ConditionTag 提供 update、copy、remove、 search、listTrash、showTrash、restore、purge。update 合并移动/重命名，restore 允许两个目标字段均省略。 调用者持有 LockedProfile 并保证 HttpTransport 及取消标志活过同步调用；每次只发送一个请求，不预读资源、 替换条件、重试写入或自动取下一页。路径参数始终是服务端 ID，不是文件路径。查询按 UTF-8 字节转义， SearchText 和名称复用领域校验；分页默认 100、上限 200，游标最长 2048 字节。

[DriveProtocol](DriveProtocol.h) 返回`Reply<variant<Node, Trash, Page<Node>, Page<Trash>, Done>>`。 Trash 是公开条目的类型化表示，包含历史原父、删除/到期时间、版本、ETag、节点数及逻辑字节数。 解析检查重复键、512KiB/深度 32 限制、规范 ID/数量/时间、ETag 与 ID/版本及响应头一致性、Location、 分页排序/重复项及请求关联；节点解析复用原有协议实现。错误只保留受检 code、状态和 request ID，不使用远端 message。合法异类 Node/Trash 条件保留后交服务端鉴权，语法错误在网络前拒绝。 读取坏响应退出 4，写入坏响应及 COMMIT_OUTCOME_UNKNOWN 退出 6；未知响应不自动变为成功。

TransferReply 中的 Upload 必有 nullable result_deleted_at，非空仅允许 COMPLETED 且不早于 terminal_at。 Storage 必有 pending_gc_bytes/objects、deleting_bytes/objects、used_nodes、reserved_nodes 和 node_limit；校验 GC 子集、节点名额及 64 位数量，不把 GC 统计再加到 retained 中。Storage ETag 不绑定派生计数。 上传 resume 始终查询服务端，即使本地已缓存 COMPLETED；永久删除结果返回 UPLOAD_RESULT_DELETED（退出 3），保留历史完成事实及本地记录，不要求原文件仍存在、不重新上传。show/complete 仍可正常读取删除证据。

[文件管理协议测试](tests/DriveClient_unittest.cc)覆盖八类请求、参数/响应绑定、条件、分页及新错误，验证响应损坏/异常不重试及 GC/节点统计拒绝规则。[传输测试](tests/TransferClient_unittest.cc)覆盖删除结果和本地完成缓存的重新确认。[真实进程测试](../../tests/integration/TransferClientProcess_unittest.cc)验证管理/传输闭环、两用户权限和重启；测试范围与运行方式见下节。

### 存储统计

Storage 解码要求隔离与不可用四项统计字段，严格解析十进制字符串，并校验 GC/隔离/不可用子集及求和溢出；缺少必需字段的响应会被拒绝。实际占用超出预算仍可读取，以支持后续检查与释放。

## 测试

```sh
cmake --build cmake-build-debug --target anof_client_protocol_test anof_client_profile_test anof_client_http_test anof_client_transfer_test anof_client_drive_test --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_client_(protocol|profile|http|transfer|drive)_test$' -j 1
```

测试覆盖 DTO/ETag/64 位数量、错误与秘密处理、profile 严格读取、权限/链接、锁、原子发布故障、目录上限，以及回环 HTTP 的实际字节预算、期限、中断、重定向、代理和不可信 TLS。使用 KiB/MiB 夹具；1 TiB 仅作为合成数量，不生成大文件。

HTTP 测试需要允许本地回环 socket；TLS 夹具使用 Python 3 标准库 ssl，无需 OpenSSL C++ 开发包。[fixtures](tests/fixtures) 中的证书/私钥仅是公开测试数据，禁止用于实际服务。局部测试不访问外网，不验证正式 HTTPS 部署或断电持久性。远程 CLI、终端交互和真实进程验收见 [命令入口说明](../../apps/anof/README.md)。
