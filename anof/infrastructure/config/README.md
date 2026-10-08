# 实例 TOML 配置

[TomlInstanceConfiguration.h](TomlInstanceConfiguration.h) 提供 `anof::infrastructure::config::makeTomlInstanceConfigurationLoader()`，返回 `Result<std::unique_ptr<InstanceConfigurationLoader>>`。链接目标为 `anof_infrastructure`，私有依赖已安装的 `tomlplusplus::tomlplusplus`。

## 构造、读取与所有权

工厂不读文件，成功返回非空对象，分配失败返回 InternalError。`load(lease)` 仅接受 [Linux 实例锁](../system/LinuxInstanceLock.h) 的活动 lease，借用其 `.anof` 目录描述符打开固定 `config.toml`，不根据外部配置指定其他路径。调用方须在整个调用期间保持 lease 存活；返回值自有字符串和值，不保留 lease、文件描述符或 TOML 节点。

文件必须为服务账户所有的普通文件，权限 0400 或 0600、无特殊权限位、硬链接数为 1；拒绝符号链接、目录、FIFO 等类型。打开与 inode 检查复用 [InstanceFiles](../system/internal/InstanceFiles.h)，目录及实例权限规则见 [system 文档](../system/README.md)。

[有界读取](../system/internal/BoundedFileRead.cc) 先检查文件大小，然后按最多 4096 字节一批读取，总内容上限 65536 字节。达到上限后最多探测一个额外字节，不把它加入内容；读取结束再次检查文件大小。短读继续、EINTR 重试，增长超过上限也拒绝。内部函数只借用 fd，loader 的 RAII 文件对象负责在正常返回及异常路径关闭；不释放调用方的实例锁。

读取边界不是文件快照：持锁期间所有写配置的维护操作也必须遵守同一锁，不支持同 UID 外部进程并发改写。大小检查不提供底层 I/O 截止时间或任意持续信号下的实时保证。

## 文件表示

仅接受六个顶层键，示例：

```toml
listen_address = "127.0.0.1"
port = 8080
development_http = true
metadata_queue = 256
http_in_flight = 128
shutdown_grace_seconds = 30
```

| 键 | 类型／范围 | 缺省行为 |
|---|---|---|
| listen_address | 字符串，仅 `127.0.0.1` | 采用公共结构默认值 |
| port | 整数，0～65535，0 为动态端口 | 同上 |
| development_http | 布尔，必须显式 true | 默认 false，不能通过校验 |
| metadata_queue | 整数，1～256 | 采用公共结构默认值 |
| http_in_flight | 整数，1～128 | 同上 |
| shutdown_grace_seconds | 整数或浮点数，有限 0～30 | 同上 |

默认值唯一来自 [InstanceConfiguration](../../application/ports/InstancePreparation.h)。解析使用精确类型读取，不把字符串、布尔或浮点数隐式转换为整数；整数先检查非负及目标类型可表示范围再缩窄。最终调用 [validateInstanceConfiguration](../../application/maintenance/PrepareInstance.h) 校验监听和资源限额，不扩大已有公共约束。最小有效文件为 `development_http = true`。

toml++ 负责语法、重复定义、UTF-8 和整数溢出检查；标准 TOML 注释、引号键、整数下划线和进制表示可用，前提是解析结果符合上述键及类型要求。拒绝未知键、表、数组、日期时间和嵌套结构作为字段值；浮点 NaN／Infinity 及越界值均拒绝。没有环境变量替换、include、额外配置搜索路径或写回默认文件。

## 失败语义

- 配置语法、类型、重复／未知键、值范围、缺失显式开发 HTTP 或超过 64 KiB：InvalidArgument。
- 不匹配的 lease：InvalidArgument；缺失文件：NotFound。
- 文件类型、所有权、权限、硬链接或打开前后身份不符合：IntegrityError。
- parser 内容不进入日志或错误消息；仅返回稳定错误码。分配失败为 InternalError，系统读取错误沿用内部 filesystemError 分类，EIO 等未分类错误为 InternalError。

load 不接触 SQLite、不调用维护、不创建实例或配置。失败保持调用方 lease；通过 prepareInstance 使用时，外层准备流程负责失败后释放 lease。真实 prepareInstance 组合及配置失败不触碰数据库的验证见 [SQLite 准备流程测试](../persistence/sqlite/tests/InstancePreparation_unittest.cc)；配置局部测试本身不代表完整服务启动验收。

## 构建与测试

[CMakeLists.txt](CMakeLists.txt) 注册生产实现和局部测试。开启 ANOF_BUILD_TESTS 时提供 `anof_config_headers_check`，只依赖 anof_application 独立编译公开头；`anof_instance_configuration_unittest` 包含 [10 个 Boost.Test 案例](tests/InstanceConfiguration_unittest.cc)，CTest 超时 30 秒。

覆盖显式 opt-in 和公共默认值、全部字段边界及严格类型、错误语法与编码、未知／重复键、数值溢出及非有限值、精确 64 KiB／超限、真实文件读取期间增长、短读／EINTR／EIO、文件拒绝、外来 lease、fd 清理和持锁保持。增长和 I/O 故障使用逐次调用的内部读取接缝，不修改进程全局函数；生产始终调用系统 read。测试使用临时私有实例和 KiB 文件，结束后清理。

```sh
cmake --build cmake-build-debug --target anof_instance_configuration_unittest anof_instance_lock_unittest anof_config_headers_check --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_instance_(configuration|lock)_unittest$' -j 1
```

Release 使用 `cmake-build-release` 和相同目标。未通过提权制造他人所有权、注入真实内存耗尽或磁盘故障，也未将完整 prepare／InstanceServer／信号停止作为局部测试结论。
