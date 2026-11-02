# HTTP 测试导航

修改协议时先选转换器测试，修改路由或回调时再看真实 HTTP 交换测试。两层有时断言同一个状态码，但前者证明字段和映射规则，后者证明请求确实经过对应入口、未提前提交任务、响应和清理顺序正确，不能仅按名称相似就删掉一层。模块职责见 [README](README.md)。

## 选择覆盖范围

[tests/CMakeLists.txt](tests/CMakeLists.txt) 通过根目录的共享 Boost.Test helper 注册 17 个可执行目标，逐目标显式设置 unit／http 标签和 30 秒 CTest 超时，浏览器安全目标另带 security 标签；这些测试使用假应用端口，真实回环网络用于验证 HTTP 组件，不等同于真实存储／runtime 集成。公开头的独立编译检查是构建目标，不作为运行测试计数。注册约定统一见[项目测试说明](../../../tests/README.md#添加或整理测试)。

| 目标与源码 | 主要覆盖 |
| --- | --- |
| `anof_http_browser_security_unittest`：[BrowserSecurity_unittest.cc](tests/BrowserSecurity_unittest.cc) | Cookie 界限与模式、Host/来源/Origin/CSRF、安全配置、登录/注册、me 实例 ID、异常清除、上传与下载 |
| `anof_http_drive_protocol_unittest`：[DriveProtocol_unittest.cc](tests/DriveProtocol_unittest.cc) | Node/Trash 条件种类、严格 JSON/查询/游标、Trash DTO、上传删除证据、GC/节点统计和错误映射 |
| `anof_http_drive_handlers_unittest`：[DriveHandlers_unittest.cc](tests/DriveHandlers_unittest.cc) | 八个类型化入口、异类条件交应用、成功头/204、过载/停止/迟到/超时、共享容量和 keep-alive |
| `anof_http_download_protocol_unittest`：[DownloadProtocol_unittest.cc](tests/DownloadProtocol_unittest.cc) | 条件优先级、重复头、Range/If-Range、Unicode 附件名、HEAD/304/416、合成 1 TiB 长度 |
| `anof_http_download_handlers_unittest`：[DownloadHandlers_unittest.cc](tests/DownloadHandlers_unittest.cc) | GET/HEAD、授权前隐藏元信息、单块所有权与 consumed、迟到/取消/停止、真实 writer 背压恢复与取消 |
| `anof_http_transfer_protocol_unittest`：[TransferProtocol_unittest.cc](tests/TransferProtocol_unittest.cc) | 上传/storage JSON、条件、游标、DTO/头与错误分类 |
| `anof_http_upload_handlers_unittest`：[UploadHandlers_unittest.cc](tests/UploadHandlers_unittest.cc) | 九个入口、HEAD、PATCH pause/resume、单块所有权、迟到/取消/停止、错误关闭及 keep-alive |
| `anof_http_account_writes_unittest`：[AccountWrites_unittest.cc](tests/AccountWrites_unittest.cc) | 五个写入口的条件/命令/可信来源、DTO/204、错误映射、收体/任务容量、强制与优雅停止、提交异常恢复及 keep-alive 隔离 |
| `anof_http_account_reads_unittest`：[AccountReads_unittest.cc](tests/AccountReads_unittest.cc) | 四个账户管理读取入口、上下文/scope、用户游标、ETag、凭据/输入/方法、应用错误、容量及超时隔离 |
| `anof_http_account_protocol_unittest`：[AccountProtocol_unittest.cc](tests/AccountProtocol_unittest.cc) | 三类 If-Match、绑定保留、账户字段/嵌套额度边界、用户分页、账户 DTO/ETag 及操作错误映射 |
| `anof_http_identity_handlers_unittest`：[IdentityHandlers_unittest.cc](tests/IdentityHandlers_unittest.cc) | 六个身份/邀请操作的真实 loopback 请求、可信来源、匿名隔离、秘密成功响应、409/429/503、方法/输入拒绝及阶段取消 |
| `anof_http_reads_unittest`：[ReadHandlers_unittest.cc](tests/ReadHandlers_unittest.cc) | 四条读取路由的真实 loopback 请求、凭据/参数、DTO/游标、方法/期限/容量及迟到结果隔离 |
| `anof_http_identity_stream_unittest`：[IdentityStream_unittest.cc](tests/IdentityStream_unittest.cc) | 公共桥接的 loopback 交换、分块响应、取消/迟到、异常、秘密缓冲和任务夹具语义 |
| `anof_http_identity_protocol_unittest`：[IdentityProtocol_unittest.cc](tests/IdentityProtocol_unittest.cc) | 身份请求与 peer 校验、游标/查询/时间边界、DTO、秘密隔离、分页大小及错误映射 |
| `anof_http_protocol_unittest`：[Protocol_unittest.cc](tests/Protocol_unittest.cc) | 凭据/内容头、JSON 结构、Unicode、字节和时间边界、完整成功对象、错误映射及 muduox 序列化 |
| `anof_http_api_unittest`：[HttpApi_unittest.cc](tests/HttpApi_unittest.cc) | 真 HTTP 健康检查、404/405、请求 ID、头部拒绝、路由限额、容量归还、keep-alive 和框架错误 |
| `anof_http_create_directory_unittest`：[CreateDirectoryHandler_unittest.cc](tests/CreateDirectoryHandler_unittest.cc) | 真 HTTP 加可控 dispatcher，验证分片/chunked/Expect/trailer、一次提交、延后结果、201/401/503/507、取消、超时、迟到结果、停止和故障注入 |
| `anof_http_public_headers_check` | 二十一个头文件在独立翻译单元中编译，检查 include 自足性 |

## 执行一个组件回归

先按[构建说明](../../../BUILDING.md#测试)准备 C++ 测试依赖与已验证的 SQLite 安装。下面从源码根目录执行，只构建并运行浏览器安全组件，另编译公开头；不会启动生产实例：

```sh
cmake --preset debug-tests
cmake --build --preset debug-tests --target anof_http_browser_security_unittest anof_http_public_headers_check --parallel 2
ctest --preset debug-tests -R '^anof_http_browser_security_unittest$' -j 1
```

Release 使用对应的 release-tests preset，顺序执行，不与 Debug 共用资源并发运行。普通 debug／release preset 关闭测试。运行整个 HTTP 组件组之前，须先构建表内全部测试目标，再以 `^anof_http_.*_unittest$` 选择 CTest，并排除 integration 标签，避免名称相近的跨模块测试混入。构建环境和完整测试命令以[项目测试入口](../../../tests/README.md#执行)为准，本页不维护第二份依赖安装步骤。

## 夹具如何控制异步过程

网络测试绑定 `127.0.0.1:0`，读取系统实际分配的端口，使用有限数据、事件定时器和显式完成 gate。gate 是由测试决定何时交付结果的门控点，可稳定检查“尚未完成时不能响应”等行为，无需阻塞 sleep。等待复用 [EventLoopWait](../../../tests/support/EventLoopWait.h)，通常上限 3 秒、轮询 1 毫秒，并保留真实调用位置的诊断。

[IdentityDispatcherFixture](tests/IdentityDispatcherFixture.h) 用 Queued、Password、FinalTransaction、Ready、Delivered 阶段模拟任务。同步拒绝不回调，接纳结果延后交付，名额到回调返回才归还，销毁句柄不自动取消。它不创建真实密码线程或事务。[UploadTestSupport](tests/UploadTestSupport.h) 和 [DownloadTestSupport](tests/DownloadTestSupport.h) 门控 prepare／feed／finish／pull，检查唯一缓冲移交、归还和迟到结果处理。

生命周期测试刻意覆盖 HTTP 名额归还而 worker 槽位仍占用、取消已排队结果、最终事务不能取消、强制／优雅停止、keep-alive 上下文隔离，以及 ready／提交／观测异常。错误 task ID 和重复 completion 是端口违约故障注入，用于验证防御路径；它们不改变“接纳后延后恰一次完成”的端口要求。writer 弱引用到期只证明这些回调没有强留请求状态，不是通用内存泄漏检测。

## 背压与集成边界

下载测试使用最多 8 MiB 的合成内容、64 KiB 输出高水位，并暂缓回环客户端读取，检查实际 write WouldBlock、部分 acceptedBytes、保留块余量、恢复发送及等待 writable 时取消。start WouldBlock 和 finish WouldBlock 没有同等覆盖；固定长度响应不发送 chunked 终结帧。普通 JSON 和上传小响应的状态机测试也不能替代真实 writer 背压证据。

协议中的 1 TiB 使用合成长度检查，不创建该大小的测试文件，也不证明大文件性能。组件 fake 和原始 HTTP 交换不能证明真实 SQLite 权限／事务、TLS 代理、浏览器 Cookie 接收或所有 worker 的退出屏障。相应流程由[集成测试](../../../tests/integration/CMakeLists.txt)、[部署测试](../../../tests/deployment/README.md)和 [Web 测试入口](../../../web/README.md)负责。
