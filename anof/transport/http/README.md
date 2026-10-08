# HTTP 接入层

`anof_http` 为创建目录和健康检查提供 HTTP 路由、协议转换、请求关联及异步生命周期适配。模块只依赖 application/domain 类型和抽象端口、muduox 的 HTTP/router/middleware 公共接口，以及实现内部使用的 nlohmann-json。它不直接访问数据库、调用同步应用用例或创建 worker。

## 接口与装配

| 接口 | 职责 |
|---|---|
| [HttpApi](HttpApi.h) | 配置路由、HTTP 容量限制、响应头策略与交换 observer |
| [CreateDirectoryHandler](CreateDirectoryHandler.h) | 收体、命令校验、异步提交、结果发送和取消清理 |
| [RequestTrace](RequestTrace.h) | 通过注入的 RequestIdentity 关联请求 ID 与 HTTP 提交／终态 |
| [readBearerCredentials / validateJsonContentHeaders](BearerCredentials.h) | 提取 Bearer、检查认证来源与内容头 |
| [parseCreateDirectoryJson](CreateDirectoryJson.h) | 有界 SAX 解析，返回自有 CreateDirectoryCommand |
| [JsonResponse](JsonResponse.h) | 成功表示、错误映射、时间格式和有限 JSON 响应 |

调用方提供 dispatcher、codec、identity、内存 ready 回调及观测 sink。handler 和 HttpApi 应共享同一个生命周期来源的 ready 回调。以下代码在 server 所属 loop 执行：

```cpp
CreateDirectoryHandler handler(dispatcher, ready, taskObserver);
HttpApi api(codec, identity, ready, handler.stream(), httpObservation);
api.configure(server);
server.start();
```

`configure` 在 `start` 前仅调用一次，配置主 loop 处理连接（额外 I/O 线程数为 0），一次安装组合 selector、全局 observer 和响应头策略。后续不能用其他 HTTP setter 覆盖它。`HttpApi` 不创建、启动或停止 server。

`handler.stream()` 和安装到 server 的回调按值保留配置，不借用 handler/HttpApi 对象本身；dispatcher、codec、identity 及用户回调引用的资源仍须保持存活。调用方先关闭业务准入，再在主 loop 调用 `HttpServer::stop`，继续驱动 loop，等待实际 HTTP 清理通知和外部任务全部完成之后才能销毁依赖和 loop。HTTP 清理完成不代表整个 runtime 停止。

## 路由与准入

| 端点 | 行为 |
|---|---|
| `POST /api/v1/nodes` | 检查 ready、Bearer 和内容头，将自有 RequestContext 绑定到注入的流式处理器 |
| `GET /health/live` | 返回 `{"status":"live"}`，不访问业务存储 |
| `GET /health/ready` | ready=true 时返回 `{"status":"ready"}`，否则为 503 错误 envelope |

未知路径返回 404；已知路径的方法不匹配返回 405 和 Allow。HEAD fallback、自动 OPTIONS 和 CORS 关闭。健康检查不调用凭据 codec，也不查询数据库。健康路由的正文限额为 0。

`HttpApiOptions` 的 `maxInFlight` 默认 128，允许 1～128；`bodySeconds` 默认 15、`responseSeconds` 默认 30，只允许设置更短的正有限期限。创建目录正文最多 65536 字节，实际 trailer 在 complete 后拒绝。路由属性与原决定合并，保留更严格的限制；server 默认解析限制及阶段期限继续生效。

全局 InFlightLimiter 同时限制健康检查和业务请求。名额在交换 finished 或最后一个属性副本释放时归还，与 worker 的任务名额分开。临时容量耗尽返回 503，不改变 ready 标志。头部拒绝采用 respond + Close，不等待正文，不发送 100 Continue。100 只表示允许继续传正文，不代表 Session 已经通过业务鉴权。

## 输入与线上表示

凭据提取拒绝重复 Authorization 或任何 Cookie；缺少、格式无效的 Bearer 返回 401。只将 token 文本传给 [SessionTokenCodec](../../application/ports/SessionTokenCodec.h)，规范 base64url 校验及摘要算法由该端口的具体实现负责。模块不验证 Session 状态，不记录 token、摘要或原始认证头。内容类型只接受 application/json 和可选唯一 UTF-8 charset，Content-Encoding 只允许缺省或单个 identity；重复歧义内容头返回 400，不支持的媒体类型或编码返回 415。

JSON 解析不构造 DOM。输入必须是含 space_id、parent_id、name 三个字符串的一层对象；缺失、未知、解码后重复键、非字符串值、额外 JSON 值或非法 Unicode 均拒绝。嵌套对象或数组在开始事件立即拒绝，比通用深度 32 上限更早终止。ID 使用已有强类型解析器；名称复用领域规则，限制为 1～255 UTF-8 字节，不 trim、折叠大小写或归一化。借用的头部和正文只在调用期间使用，返回值拥有自己的数据。

成功对象只包含 id、space_id、parent_id、name、kind、created_by、created_at、updated_at。时间固定 UTC 六位微秒，支持公历年份 0001～9999，正确处理 epoch 之前的负微秒。Location 为 `/api/v1/nodes/<id>`，ETag 为带双引号的 `"node-<id>-1"`。父 ID 缺失、revision 非 1、创建和更新时间不一致、名称或时间无法表示时返回 InternalError；不能据此认定已经提交的事务被撤销。

所有模块生成的 JSON 响应带 application/json、Cache-Control: no-store、X-Request-ID；错误体使用同一请求 ID，details 固定为空对象，401 另带 Bearer challenge。应用错误与协议错误集中映射，CommitOutcomeUnknown 保留独立 code；模块不自动重试。框架错误、HEAD 和连接中止不保证 JSON，不能无条件解析响应正文。

`JsonResponse` 将响应头和正文分开保存，不自行设置 Content-Length 或 Transfer-Encoding；`buffered(close)` 生成有限缓冲响应。异步 handler 将已准备的正文和确定长度交给 HttpStreamSender，producer 的 nullopt 仅表示 EOF。

## 异步状态与观测

每请求状态由框架流式回调持有，包含自有上下文、有界正文、任务句柄、writer 和 sender。完整输入只处理一次：拒绝 trailer、解析命令、再次检查 ready，随后最多提交一次任务。同步 QueueFull/Stopping 返回 503，InternalError 返回 500，拒绝后没有完成回调。

接纳后的完成闭包只弱引用请求状态，独立保存请求 ID 和业务 observer，不捕获 writer。完成在所属 loop 转换和发送；CancelledBeforeStart 在 HTTP 仍活动时返回 503。sender 独占 writable 回调并处理部分接受和 WouldBlock，请求状态保留它至结束或取消。完成处理抛异常时显式 abort，不能追加第二份响应。

cancel 幂等释放正文、尝试 cancelBeforeStart 并取消 sender。AlreadyRunning 不会中断事务，AlreadyTerminal 不代表任务容量已归还。HTTP 状态释放后仍消费迟到结果；DirectoryTaskObserver 接收 request ID、task ID、可选 attempt、取消／错误分类及交付时的 HTTP 活动状态。观测异常被隔离，不改变业务结果或阻止响应。

RequestTrace 使用 exchange 编号通过注入的 RequestIdentity 获得服务端请求 ID，忽略客户端同名头，无全局交换表。记录不包含请求目标、query、凭据或正文。committed 和 finished 分别反映实际 HTTP 提交和终态，不证明客户端收到响应；响应头策略失败的底层兜底也不保证关联头。所有 sink 必须非阻塞且有界，模块不提供日志队列或事务审计。

muduox 在完整收体后等待结果时暂停读取，因此远端 FIN 不保证立即可见。取消来自实际已观察的断开、超时或停止等事件；断连不证明未运行或未提交。body 超时当前为 408，等待结果的 response 超时为框架 500，不是 504；定时器不能中断外部 worker 的阻塞操作。

## 构建与验证

[CMakeLists.txt](CMakeLists.txt) 定义静态库；[tests/CMakeLists.txt](tests/CMakeLists.txt) 在 ANOF_BUILD_TESTS 开启时注册三个 Boost.Test 可执行文件，每个 CTest 超时为 30 秒，并为六个公开头生成独立编译检查。

| 目标与源码 | 验证范围 |
|---|---|
| `anof_http_protocol_unittest`：[Protocol_unittest.cc](tests/Protocol_unittest.cc) | 凭据／内容头、JSON 结构、Unicode、字节和时间边界、完整成功对象、错误映射及 muduox 序列化 |
| `anof_http_api_unittest`：[HttpApi_unittest.cc](tests/HttpApi_unittest.cc) | 真 HTTP 健康检查、404/405、请求 ID、头部拒绝、路由限额、容量归还、keep-alive 和框架错误 |
| `anof_http_create_directory_unittest`：[CreateDirectoryHandler_unittest.cc](tests/CreateDirectoryHandler_unittest.cc) | 真 HTTP 加可控 dispatcher，验证分片/chunked/Expect/trailer、一次提交、延后结果、201/401/503/507、取消、超时、迟到结果、停止和故障注入 |
| `anof_http_public_headers_check` | 六个头文件在独立翻译单元中编译，检查 include 自足性 |

从仓库根目录复用 Debug 或 Release 构建目录，串行运行 CTest：

```sh
cmake --build cmake-build-debug --target anof_http_protocol_unittest anof_http_api_unittest anof_http_create_directory_unittest anof_http_public_headers_check --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_http_(create_directory|api|protocol)_unittest$' -j 1
```

网络测试使用 127.0.0.1 端口 0 并读取实际监听地址，使用小 JSON、显式完成 gate 和有限 timer，不使用固定 sleep。它们覆盖 Queued、Running、AlreadyTerminal、重复／优雅停止、超时后 CommitOutcomeUnknown、ready/提交/观测/响应头异常。writer 弱引用过期约束请求状态不得被迟到回调强保活，但不等同于一般内存泄漏检测。提交异常只模拟接纳前失败，不支持“接纳后抛出且不给句柄”的违约 dispatcher。

正常小响应低于 writer 的 1 MiB 高水位，测试不声称触发真实 WouldBlock。当前 HttpServer 没有公开容量注入接口；背压恢复、发送中取消和 producer 异常不能由这些普通请求证明。测试使用注入依赖，不验证真实 crypto、SQL 事务、完整 runtime 双侧关闭屏障或物理机持久性。
