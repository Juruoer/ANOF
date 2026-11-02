# 请求、身份与传输设计

Web 是同源浏览器客户端。它把用户的操作转成 HTTP 请求，验证返回的数据，再更新当前身份下的视图；权限、额度和文件发布由服务器决定。本文面向修改客户端的人，使用流程见 [README](README.md)，验证入口见 [TESTING](TESTING.md)。

一次上传可以说明各层如何配合：传输视图收集目标和 File，TransferStore 先核对身份、取得记录锁，再让 Worker 计算摘要。记录库保存创建责任后，TransferApi 才发送请求。服务器确认的偏移更新到本地记录，视图显示进度；如果响应丢失，编排停止写入并查询，交还用户决定下一步。

## 从视图到共享请求

[src/main.ts](src/main.ts) 创建一个 ApiClient 和 IdentityStore，按 hash 路由挂载账户、文件、管理员或传输视图。各业务目录中的 `protocol.ts` 解码数据，`api.ts` 组合端点调用，`store.ts` 持有业务状态，`views.ts` 构造 DOM。目标目录选择复用 [DestinationPicker](src/drive/picker.ts)。

所有适配器共享 [ApiClient](src/api/client.ts) 的六个在途请求名额和一个写入名额，不能为上传另建无限请求池。准入失败立即返回 busy；请求在 finally 中释放名额。`waitForMutation` 最多等待五秒，用于本地传输取消后让登出、改密取得写槽。这个等待只覆盖浏览器请求退出，不证明服务端工作已停止。

请求只允许 `/api/v1/` 同源相对路径，使用浏览器 Cookie，禁止重定向，不自动重试。非 GET/HEAD 请求带 `X-ANOF-Request: 1`；Origin 由浏览器生成，代码不设置 Authorization。`referrerPolicy: same-origin` 避免某些浏览器发送空 Origin。会话 Cookie 的 HttpOnly 属性意味着 JavaScript 无法读出或自行清除它。

JSON 请求最多 64 KiB，响应最多 512 KiB。读取响应时按实际字节限制容量，并检查声明长度、JSON 媒体类型和 UTF-8；不能只信 Content-Length。解码器校验 ID、实例绑定、状态、修订和字段范围。字节数及修订使用规范十进制字符串和 BigInt，ETag 按服务器返回的完整引号形式随 If-Match 发送，不经浮点数或自行拼补修订。

默认请求期限为 45 秒，也包含读取响应正文。HEAD 是只读请求，失败响应可能无正文，只按状态报告，不能捏造 JSON 错误码。写入发生网络错误、5xx、未知提交或无法解码的成功响应时保留“结果未知”；业务拒绝与未知结果分开处理。412 后刷新当前资料，需要用户重新决定；读回匹配值不等于确认前次写入成功。

[src/ui/dom.ts](src/ui/dom.ts) 使用文本节点展示名称、错误和进度。不能把服务器内容插入 HTML，也不能把密码、邀请码或会话凭据放进 URL、持久状态或诊断。

## 身份变化与视图寿命

[IdentityStore](src/auth/store.ts) 只有在 GET me 验证完成后才绑定 origin、instance_id 和 user_id。登录响应不替代这次身份核对，注册也不复用提交的密码自动登录。普通业务 401 使本地身份失效。

IdentityStore 与 ApiClient 的代次检查使旧读取不能回填已销毁的身份。main 在身份进入核对阶段、执行身份写入、绑定改变或离开路由时销毁相应受保护视图。视图销毁取消读取和本地任务，业务 store 也拒绝晚到结果；文件和管理员 store 的写入结果不能在身份销毁后触发新的核对或重新显示资料。

`visibilitychange` 回到可见状态及持久 `pageshow` 会自动复验正常登录身份，`pagehide` 则使本地身份失效。但登出后自动恢复门控关闭：即使 DELETE 未获确认、Cookie 仍有效，也不会因这些自动事件恢复视图或重发 DELETE。用户主动核对或登录成功才重新开放门控。完整 reload 建立新的 store，仍会重新引导身份。

登出和改密先停止受保护视图中的工作，再有限等待写槽退出。取消本地请求不是服务端回滚；身份恢复也不会自动重启上传。管理员邀请秘密另有一次性消费、两分钟期限及隐藏/离开清理，邀请元信息不能恢复已清除的秘密。

## 上传如何推进

[TransferStore](src/transfer/store.ts) 保留最多 16 个排队 File 引用，一次处理一个文件。队列尚未开始的文件不预读，前一项待处理或需要目标决策时停止后续调度。`start`、`resume`、检查和取消等入口在执行前通过当前 GET me 验证身份。

### 预哈希与创建责任

[hashFile](src/transfer/hash.ts) 创建一个模块 Worker，[Worker](src/workers/sha256.ts) 使用本地 hash-wasm 的增量 SHA-256。每次只读取一个至多 1 MiB 的 `file.slice`；发出进度后等主线程确认才读取下一块。这种背压限制未确认的块和消息，不代表浏览器进程 RSS 上限。每块等待 30 秒、整次最多六小时；错误、超时或取消会移除监听、结束 Promise 并 terminate Worker。

编排先生成随机 client_upload_id，完成预哈希后将它与摘要、目标一起保存为创建责任。首次 create POST 之前必须已有持久记录，发送前标成 UNCERTAIN 并从可自动创建的队列移出。零字节文件使用空内容摘要，不发送 PATCH。未知创建通过原 client_upload_id 查询并验证绑定，即使未找到也不换 ID、不自动重发 POST。

### 分块与发布

[TransferApi](src/transfer/api.ts) 使用已知长度的 Blob slice，PATCH 媒体类型为 `application/offset+octet-stream`，带 Upload-Offset。默认每块 1 MiB，不能超过服务器公布值或 8 MiB；不走 JSON、multipart 或请求流，不手工设置浏览器禁止的 Content-Length。PATCH 期限为 375 秒，为服务器 body 300 秒、response 60 秒留余量；控制与查询请求为 45 秒。

只有 PATCH 204 确认的偏移恰等于块起点加块长度才前进。File 长度和偏移先经 1 TiB 上限及安全整数检查，再转换为 slice 需要的 Number。哈希和已发送字节都不能作为服务器已持久接收的依据。

| 服务器状态 | 编排行为 |
| --- | --- |
| CREATING | 已接受创建，等待查询结果，不重复创建 |
| OPEN | 从核对后的偏移串行 PATCH，完整接收后发起 complete |
| VERIFYING、COMMITTING | 查询验证或发布进度，不重复 complete |
| READY 且无错误 | 作为发布过程中的过渡状态继续只读查询 |
| READY 且有错误 | 等待用户选择同空间目录和名称后再次完成，不自动覆盖或改名 |
| COMPLETED | 验证结果节点并显示发布确认；result_deleted_at 非空则显示已删除的历史结果，不重建 |
| CANCELLED、EXPIRED、FAILED | 展示终止状态，不把它变成新上传 |

等待状态最多查询 20 次、总窗口 60 秒，单次读取也受剩余窗口约束；窗口结束后由用户稍后检查。202 表示已接纳，不能显示发布完成。协议层还验证不可变绑定、摘要、终态时间及结果字段之间的一致性。

### 恢复和停止

恢复必须由用户重选 File，完整重算长度和摘要。随后 GET 返回的上传和 HEAD 的长度、状态、偏移、有效期必须一致；本地 confirmed_offset 只是线索。任何不一致都停止，不猜测服务器位置。404、过期或失败不自动替换成新上传。

未知 PATCH、complete、cancel 后先停止写入，尝试读取服务器状态并保存线索；没有自动写入重放。暂停中止本地等待，离开视图还清空队列和监听器。停止本地 Worker、释放锁之后，服务器仍可能清理已发请求或执行已接纳的验证任务，不能根据 AbortController 推断回滚。

本地记录删除与服务器 DELETE 是不同操作，前者不释放远端预留。服务器取消 204 确认逻辑取消，不确认物理清理完成；已经终态的记录只核对保存，不再次取消。缺本地记录时仍可使用服务器列表检查或明确取消，列表每页 100 项、最多四个返回游标。

## 本地记录与标签页互斥

[Records](src/transfer/records.ts) 使用 IndexedDB `anof-transfers`，数据库和 record_version 都为 1。records store 必须以 `[origin, instance_id, user_id, client_upload_id]` 为复合 keyPath，且 autoIncrement=false。每次事务先验证结构，再用游标校验记录和主键；缺 store、版本或结构不支持、内容损坏都拒绝操作并保留现场，不静默重建。

记录只含以下字段，不保存 File 字节、句柄、磁盘路径、密码或凭据：

| 字段 | 用途 |
| --- | --- |
| record_version | 记录解码版本 |
| origin、instance_id、user_id、client_upload_id | 身份与本地创建责任的复合键 |
| upload_id | 已核对的服务器上传 ID，创建未确认前可以为空 |
| space_id、parent_id、name | 目标位置和名称 |
| size、sha256、confirmed_offset | 原文件校验与恢复线索 |
| state | 本地 INITIAL/UNCERTAIN 或服务器状态 |
| created_at、updated_at | 浏览器本地记录时间，终态清理使用固定的 updated_at |

单条编码最多 4 KiB，全库最多 64 条、256 KiB。读取遇到第 65 条就拒绝；所有记录校验、过期清理、容量检查和变更在同一个 readwrite 事务完成，防止两个标签页以过期内存计数同时占据最后一个位置。数据库打开和事务各有十秒期限。

created_at/updated_at 不与服务器 terminal_at 比较。活动记录更新取当前本地时间与已有 updated_at 的较大值，容忍回拨；首次确认终态后固定更新时间，后续核对不续期。访问库时清理已达到七天保留期限的终态，活动及未知记录不自动淘汰。浏览器墙钟跳变会影响清理时机，不能把这个策略当作独立于时钟的后台定时服务。

初次保存失败就不创建远端上传；后续保存失败停止继续写入，保留可核对的远端结果，不自动另建。UI 刷新本地记录时，如果内容没有变化就保留原表单节点，避免晚到刷新清掉用户刚重选的文件。

[withRecordLock](src/transfer/locks.ts) 用 origin/instance/user/client_upload_id 标识 Web Lock，在恢复、哈希、写入、核对及本地工作退出期间持有。`ifAvailable` 使冲突立即返回，不排队接管，也不以时间戳锁代替。不支持该 API 时禁用上传写入和恢复。

## 原生下载与静态资产

文件下载使用同源节点 ID 的 anchor，由浏览器携带 Cookie，服务器执行内容授权并返回 attachment/filename*。客户端没有 fetch-to-Blob、JavaScript 下载流、凭据 URL 或本地文件系统下载器。元数据可见不替代内容端点授权，点击链接不等于收到内容。

Vite 将 Worker 和 hash-wasm 的 WASM 随本地资产打包，并复制 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)。静态页面及 Worker 的 CSP 需要 `script-src 'self' 'wasm-unsafe-eval'` 和允许同源 Worker；不需要 JavaScript unsafe-eval、内联脚本例外或 CDN。部署模板与运行配置统一见[部署说明](../deploy/README.md)。

[vite.config.mjs](vite.config.mjs) 的 dev/preview 服务拒绝 API 和 health 路径，避免静态 HTML 吞掉后端错误。开发服务还限制 Host、跨源访问和可服务文件范围；它不承担生产 HTTPS 或后端代理职责。
