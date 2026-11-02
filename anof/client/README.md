# HTTP 客户端库

`anof_client` 为 C++ 调用者提供同步、类型化的远程 API、profile/凭据存储和文件传输。它通过 HTTP 访问
ANOF，不读取服务端数据库。需要直接使用命令行时，请阅读 [CLI 说明](../../apps/anof/README.md)；本库不是独立客户端可执行程序。

例如一个命令行调用先锁定 profile，读取其中的服务地址和凭据，再向 `ApiClient` 发起同步调用。库负责构造请求、验证响应和保存必要状态，调用者负责交互与展示结果。

## 接入

使用 CMake 目标 `anof_client`。依赖领域值、libcurl、nlohmann-json 和 libsodium，不链接服务器 runtime、SQLite 或
muduox。编译依赖与通用入口见 [构建说明](../../BUILDING.md)。

| 接口                                     | 用途                             |
|------------------------------------------|----------------------------------|
| [ProfileStore](ProfileStore.h)           | profile 与持锁凭据访问           |
| [ApiClient](ApiClient.h)                 | 身份、账户、空间和目录 API       |
| [DriveClient](DriveClient.h)             | 移动、复制、搜索和回收站         |
| [TransferClient](TransferClient.h)       | 上传、下载及显式续传             |
| [CurlHttpTransport](CurlHttpTransport.h) | 有界 HTTP 传输，可由测试替身替换 |

调用前取得 profile 锁；transport、profile 与取消标志需存活到同步调用结束。库只使用 Bearer，不启用浏览器
Cookie。未知写结果须查询确认，不能自动重发；取消也不能证明服务端回滚。接口示例、响应验证、凭据文件权限及续传语义见 [客户端设计](DESIGN.md)。

## 验证

[本地测试](tests/CMakeLists.txt)覆盖协议、凭据和传输边界，[进程集成测试](../../tests/integration/CMakeLists.txt)覆盖实际
CLI/服务交互。运行方式见 [测试说明](../../tests/README.md)。
