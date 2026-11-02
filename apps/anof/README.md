# ANOF 命令行

统一的 `anof` 程序提供实例初始化、前台服务、本地运维和远程客户端命令。远程命令通过 HTTP 访问服务，不要求本机另启服务器。

## 构建与启动

从项目根目录按 [构建说明](../../BUILDING.md) 准备依赖并执行：

```sh
./scripts/build.sh --preset release
./cmake-build-release/apps/anof/anof --help
```

用 `anof --version` 查看客户端程序版本，`anof serve --version` 查看服务器上本机程序的版本；两者不访问实例或远程服务。

程序位于 `cmake-build-release/apps/anof/anof`。安装、创建实例与 HTTPS 部署见 [部署说明](../../deploy/README.md)。`init` 要求已有空的 0700 目录，密码在终端无回显输入；`serve` 不会自动创建实例。

```sh
anof init --help
anof serve --root /absolute/path/to/instance
```

`anof` 表示本次构建或安装的程序。服务报告 `ANOF_READY` 后才可使用；停止发送 SIGINT 或 SIGTERM，等待清理完成。请求中断后的处理见命令参考中的错误说明。

## 使用远程实例

每个远程命令显式选择 profile，公共选项置于子命令之前：

```sh
anof --config-dir /absolute/path/to/client-config profile add family --server https://cloud.example.test
anof --config-dir /absolute/path/to/client-config --profile family login --login admin
anof --config-dir /absolute/path/to/client-config --profile family --json me
```

profile 保存服务地址；登录后本地凭据目录应仅由当前用户访问。密码不放入参数、环境变量或脚本。生产环境使用 HTTPS 和有效证书；开发 HTTP 必须显式启用且仅用于回环环境。

账户、额度、目录、上传/下载、续传、搜索和回收站的具体用法，以及初始化中断、错误与退出码，见 [命令参考](COMMANDS.md)。未知写结果需要先查询，不能按普通网络失败自动重试。

## 本地维护与开发

配置检查、管理员密码重置、FSCK、受限修复、停机备份及新目录恢复也由此程序提供。维护前停止服务并确认实例锁可取得；修复原始副本与产品备份不能混用。命令参数见 [本地运维](COMMANDS.md#本地运维)，部署流程见 [部署说明](../../deploy/README.md)。

要修改命令解析、输出或进程控制，可以继续阅读 [组件与测试](DESIGN.md)、[runtime](../../anof/runtime/README.md) 和 [客户端库](../../anof/client/README.md)。
