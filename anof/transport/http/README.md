# HTTP 接入层

transport 把 HTTP 请求转换为应用命令，再把异步完成结果转换为响应。它负责路由、输入格式、浏览器来源保护和网络发送；身份授权、数据库事务、密码计算与文件操作交给应用及 runtime。库目标是 `anof_http`，依赖类型化端口和 muduox 的 HTTP、router、middleware 接口，不创建工作线程。

以创建目录为例，`HttpApi` 先检查来源、凭据和内容头，再把请求交给 `CreateDirectoryHandler`。处理器收齐 JSON 后构造命令，通过任务调度端口（dispatcher）提交。runtime 在元数据线程执行用例，将结果送回 HTTP 所属的事件循环，处理器才发送 201 和新目录信息。如果等待期间请求超时，HTTP 停止发送，但已开始的事务仍可能完成。

## 从哪里开始

| 你要做的事 | 阅读入口 |
| --- | --- |
| 将 HTTP 接到服务中 | 下方装配示例、[HttpApi.h](HttpApi.h)、[runtime](../../runtime/README.md) |
| 查找请求字段、路由、条件和响应 | [HTTP 协议](WIRE.md) |
| 调整凭据或代理接入 | [凭据与同源保护](SECURITY.md) |
| 修改异步回调、上传或下载 | [请求与流的生命周期](STREAMS.md) |
| 选择回归测试 | [测试导航](TESTING.md) |

服务器运行、HTTPS 代理和 Web 部署见[部署说明](../../../deploy/README.md)；本目录面向调用和维护 C++ 接入层的开发者。

## 接到已有服务

调用方先准备 dispatcher、token codec、请求 ID 生成器、内存中的 `ready` 回调，以及经过校验的实例配置。目录处理器与 HttpApi 应使用同一就绪状态。下面是装配片段，所有名称代表调用方已经创建的对象，在 server 所属事件循环上执行；它不是独立程序：

```cpp
CreateDirectoryHandler handler(directoryDispatcher, ready, taskObserver);
HttpApi api(codec, requestIdentity, ready, handler.stream(),
            identityDispatcher, transferDispatcher, driveDispatcher,
            instanceConfiguration, httpObservation);
api.configure(server);
```

随后由调用方启动监听。生产服务的监听失败处理与停止顺序由 [InstanceServer](../../runtime/InstanceServer.cc) 负责。`HttpApi::configure` 只能在启动前调用一次；它安装组合选择器、响应头策略和交换观察器，并设置额外 I/O 线程数为 0。装配后不要再用其他 setter 覆盖这些策略。HttpApi 自身不启动、停止 server，也不退出事件循环。

`InstanceConfiguration` 的安全值被复制到策略中，配置对象不必随请求存活。dispatcher、codec、请求 ID 生成器和回调引用的其他资源则必须活过 HTTP 清理与所有外部任务的完成处理。`handler.stream()` 及 server 安装的回调不借用 handler 或 HttpApi 对象本身。

停止时先关闭新业务准入，再调用 `HttpServer::stop`。继续驱动事件循环，等待 HTTP 清理通知和外部任务完成，最后销毁依赖。如何处理运行中的事务、迟到结果和文件缓冲，见[停止与取消](STREAMS.md#停止与取消)；整个服务的 worker 退出及实例锁寿命见 [runtime 设计](../../runtime/DESIGN.md#停止顺序和锁的寿命)。

## 代码如何分工

[HttpApi.cc](HttpApi.cc) 注册路由并选择处理方式。[BrowserSecurity](BrowserSecurity.h) 与 [RequestCredentials](RequestCredentials.h) 完成同步入口检查；请求格式由 `IdentityRequest`、`AccountRequest`、`DriveRequest`、`TransferRequest` 和对应的 Response 文件转换。公共值和命令来自[应用端口](../../application/ports/README.md)，HTTP 类型不进入业务用例。

大多数控制请求使用 [makeIdentityStream](IdentityStream.h)，由一份每请求状态负责收体、提交和发送。创建目录保留 [CreateDirectoryHandler](CreateDirectoryHandler.h) 的事务诊断接口。大块数据走 [UploadStream](UploadStream.h) 和 [DownloadStream](DownloadStream.h)，让网络与文件任务之间始终只有有限缓冲。

[RequestTrace](RequestTrace.h) 把交换与服务端请求 ID 关联，向调用方提供提交和结束事件。它只输出静态路由模板，不输出原始路径、查询、凭据或正文；观测回调必须快速返回且限制存储量。日志队列和业务审计由其他模块负责。
