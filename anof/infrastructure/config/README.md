# 实例配置

配置适配器把实例的 `config.toml` 读成自有的 `InstanceConfiguration`。启动时，`prepareInstance` 已经取得实例锁，再调用 loader 读取监听、队列、磁盘余量和浏览器安全配置；通过公共校验后，才进入数据库维护。配置读取失败不会触碰数据库。

入口 [makeTomlInstanceConfigurationLoader](TomlInstanceConfiguration.h) 只构造对象，不读取文件。`load(lease)` 借用活动 Linux lease 的 `.anof` 目录描述符，读取固定的 `config.toml`；返回后不保留 lease、文件描述符或 TOML 节点。调用者始终持有锁，具体路径和权限规则见[实例启动契约](../InstanceStartupContract.md#路径和权限)。

## 文件内容和模式

下面是完整的本地开发 HTTP 配置示例，不启用浏览器 Web。它用于说明解析格式；初始化命令会写出配置，无需手工创建实例文件。部署 HTTPS 和 Web 时使用[部署说明](../../../deploy/README.md)中的配置流程。

```toml
listen_address = "127.0.0.1"
port = 8080
development_http = true
metadata_queue = 256
http_in_flight = 128
shutdown_grace_seconds = 30
disk_safety_min_bytes = 1073741824
disk_safety_percent = 5
web_enabled = false
public_origin = ""
trusted_proxies = []
audit_retention_days = 90
audit_batch_size = 200
```

解析器只接纳这十三个顶层键。默认值来自公共 [InstanceConfiguration](../../application/ports/InstancePreparation.h)，最终模式与资源校验由 [validateInstanceConfiguration](../../application/maintenance/PrepareInstance.cc) 完成，适配器不维护另一套默认值。

| 字段 | 接纳条件 |
|---|---|
| `listen_address` | 字符串，精确为 `127.0.0.1` 或 `::1` |
| `port` | 整数 0～65535；0 表示由系统分配端口 |
| `development_http`、`web_enabled` | 布尔值；开发模式要求 Web 关闭 |
| `public_origin` | 开发模式为空；HTTPS 代理模式要求规范 HTTPS origin |
| `trusted_proxies` | 开发模式为空；HTTPS 代理模式要求一至两个不重复的精确回环地址 |
| `metadata_queue`、`http_in_flight` | 分别为整数 1～256、1～128 |
| `shutdown_grace_seconds` | 有限整数或浮点数，0～30 秒 |
| `disk_safety_min_bytes`、`disk_safety_percent` | 分别为整数 0～INT64_MAX、0～100 |
| `audit_retention_days`、`audit_batch_size` | 分别为整数 7～3650 天、1～1000 条 |

HTTPS 代理模式即使未启用 Web，也必须提供 origin 和可信代理。origin 不含路径、用户信息、查询或片段，使用小写 DNS、规范 IP 和省略默认 443 端口的形式；规则由 [PublicOrigin](../../domain/identity/PublicOrigin.h) 定义。这里只校验配置，请求来源、Cookie 和 CSRF 由 HTTP 入口执行检查。

普通加载可以省略具有默认值的字段，但最终组合必须合法。最小开发配置为 `development_http = true`；空文件会采用 HTTPS 模式的默认值，因缺少 origin 和代理而失败。内容预算属于 SQLite 设置，`content_budget_bytes` 不是 TOML 键，也不会根据测试机器磁盘大小自动调整余量。

## 读取和错误处理

读取最多 65536 字节，先检查大小，以最多 4096 字节一批读取，并在结束后复查大小；达到上限只再探测一个字节。短读继续、EINTR 重试，读取期间增长到超限同样拒绝。文件句柄由局部 RAII 对象关闭，不释放调用方的实例锁。这些检查不提供文件快照：持锁期间改配置的程序也必须遵守同一锁协议。

toml++ 负责语法、UTF-8、重复定义与整数溢出检查。适配器按精确类型取值，不把字符串、布尔或浮点隐式转成整数；NaN、Infinity、未知键和不符合字段类型的嵌套结构均拒绝。`trusted_proxies` 是唯一接受数组的字段，只允许字符串元素。标准 TOML 注释、引号键、整数下划线及进制表示可以使用。没有环境变量替换、include、配置搜索路径或默认值写回。

| 失败 | 返回结果 |
|---|---|
| 语法、类型、模式组合、范围或 64 KiB 上限不合法 | `InvalidArgument` |
| lease 不属于此 Linux 适配器 | `InvalidArgument` |
| 必需文件缺失 | `NotFound` |
| 类型、权限、所有权、硬链接或打开前后文件身份不符 | `IntegrityError` |
| 分配失败、未分类的读取故障 | `InternalError` |

错误只包含稳定分类，不传播 parser 的原始诊断或配置内容。直接调用 loader 失败后锁仍由调用者持有；通过 `prepareInstance` 调用时，由外层准备流程释放失败路径的 lease。

## 初始化和恢复写入

内部 [ConfigurationText](internal/ConfigurationText.h) 共用解析、校验和序列化。序列化写出全部十三键，使用固定数值 locale 和可往返的 double 精度。新实例的配置由初始化 lease 发布；重新打开 pending 时必须有完整十三键配置，不能以默认值补齐半成品。

恢复使用 [loadExplicitInstanceConfiguration](TomlInstanceConfiguration.h) 读取调用者提供的独立文件。路径必须是规范绝对路径，父目录受控，文件满足私有普通文件规则；仍限制 64 KiB，并要求完整十三键。恢复不会从备份静默继承监听地址、origin 或代理。文件创建和同步由 system/存储适配器负责，config 本身不持有数据库或执行发布。

## 修改时检查什么

[配置测试](tests/InstanceConfiguration_unittest.cc) 覆盖字段、精确类型、模式组合、大小边界、短读/增长/EINTR/EIO、危险文件、描述符清理和锁寿命。配置完整序列化与初始化重开还由[初始化文件测试](../system/tests/InstanceInitializer_unittest.cc)覆盖；启动短路顺序见[准备流程测试](../persistence/sqlite/tests/InstancePreparation_unittest.cc)。测试注册入口是 [CMakeLists.txt](tests/CMakeLists.txt)，统一运行方法见 [BUILDING](../../../BUILDING.md#测试)。
