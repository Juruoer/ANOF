# Crypto 适配器

本目录通过 application 端口提供 token 摘要计算、请求 ID 派生和随机节点 UUID 生成。公开头文件不暴露 libsodium 类型，不依赖 HTTP、runtime、SQLite 或测试 fixture。

| 头文件 | 工厂 | 接口可用性 |
|---|---|---|
| [SodiumSessionTokenCodec.h](SodiumSessionTokenCodec.h) | `makeSodiumSessionTokenCodec()` | 已定义，返回 `SessionTokenCodec` |
| [SodiumRequestIdentity.h](SodiumRequestIdentity.h) | `makeSodiumRequestIdentity()` | 已定义，返回 `RequestIdentity` |
| [SodiumNodeIdGenerator.h](SodiumNodeIdGenerator.h) | `makeSodiumNodeIdGenerator()` | 已定义，返回 `NodeIdGenerator` |

命名空间为 `anof::infrastructure::crypto`，工厂返回类型统一为 `Result<std::unique_ptr<端口>>`。链接目标为 `anof_infrastructure`。

## SessionTokenCodec 用法

在启动期间调用 `makeSodiumSessionTokenCodec()`，检查 Result 成功后取出独占对象。失败时不得发布 READY；成功对象由装配方持有到 HTTP 清理及迟到任务观测结束。

```cpp
#include <anof/infrastructure/crypto/SodiumSessionTokenCodec.h>

// 启动阶段，调用方可把错误继续返回给装配层。
auto codec = anof::infrastructure::crypto::makeSodiumSessionTokenCodec();
if (!codec) return codec.error();
// tokenText 为不含 Bearer scheme 的凭据文本。
auto digest = codec.value()->digestBearer(tokenText);
```

[digestBearer 实现](SodiumSessionTokenCodec.cc) 只接受 32 字节 token 的规范无 padding base64url 表示：恰好 43 字符，字母表为 `A-Z a-z 0-9 - _`，最后一个字符的低两位为零。不接受空白、NUL、`=`、额外尾随数据或数据库摘要文本。输入无需以 NUL 结尾，只有 string_view 指定的内容参与处理。

校验后使用 libsodium 解码，确认完整消费输入且输出恰好 32 字节，再调用 `crypto_hash_sha256`。SHA-256 输入是解码字节，不是编码文本，也不使用 `crypto_generichash`。返回的 SessionDigest 由调用方交给身份查询端口；codec 不执行 Session 查询，不判断到期、撤销或账户状态，也不签发 token。

非法 token 返回 `Unauthenticated`。输入已通过规范性验证后，解码后置条件异常或哈希失败返回 `InternalError`。对象无可变请求状态，可供 loop 侧调用；每次只处理固定上限的输入与栈缓冲区，无初始化、数据库、文件 I/O、密码哈希或堆分配。退出时清零适配器持有的解码 token 缓冲区；不持有或清除调用方原始输入，也不承诺清除所有寄存器副本。token、摘要均不写日志。

## RequestIdentity 用法与派生规则

启动时调用 `makeSodiumRequestIdentity()`，检查成功后为整个实例复用同一个 provider，持有至 HTTP 清理和迟到任务观测结束。每次调用工厂均创建独立 provider；不要按请求重新构造。

[生产实现](SodiumRequestIdentity.cc) 先完成 libsodium 初始化，再通过 `randombytes_buf` 取得 32 字节启动种子。种子只保存在对象内部，不可通过生产构造参数指定，不持久化、不写日志；对象析构时使用 sodium_memzero 清零。随机接口不返回错误码，库级致命失败不能转成 Result；不会退回固定种子。随机 API 见 [libsodium 文档](https://doc.libsodium.org/generating_random_data)。

`forExchange(connection, request)` 的[内部派生](internal/RequestIdDerivation.cc) 对以下 66 字节作 SHA-256：

| 字节偏移 | 内容 |
|---|---|
| 0～17 | ASCII `ANOF/request-id/v1`，不含 NUL |
| 18～49 | 32 字节启动种子 |
| 50～57 | connection，8 字节大端 |
| 58～65 | request，8 字节大端 |

取摘要前 16 字节，保留原有 version／variant 位，按小写 UUID 分组解析为强类型 RequestId。仅当这 16 字节全零时令最后一字节为 1；摘要后 16 字节不影响此判断。编号 0 和 UINT64_MAX 均有效，相同 provider 的相同编号对始终产生相同 ID。

调用期间不访问随机源、文件或数据库，不保存交换编号映射；只读实例种子，使用固定大小栈缓冲区，正常路径无堆分配。派生缓冲区含种子副本，RAII 在正常及异常退出时清零。并发只读调用不改变实例状态，销毁仍须等待所有调用结束。

`forExchange` 没有 Result 错误通道：内部哈希失败抛出 `std::runtime_error`，格式化不变量失败抛出 `std::logic_error`，消息不含种子或输入。调用方需走异常处理路径，不能以正常外观的 ID 降级。这些防御分支不代表生产 SHA-256 通常会失败；库级终止无法通过异常恢复。

RequestId 只用于服务端关联，不是认证凭据、节点 ID 或幂等键。不同启动种子不构成绝对无碰撞保证，有限样本也不能证明随机源质量。

## NodeIdGenerator 用法与位布局

在元数据 worker 内调用 `makeSodiumNodeIdGenerator()`，检查 Result 成功后把独占对象交给 worker 资源集合，并在该线程使用和销毁。构造先完成共享 libsodium 初始化；初始化失败或对象分配失败返回 `InternalError`。

[生产实现](SodiumNodeIdGenerator.cc) 的 `next()` 每次调用 `randombytes_buf` 获取 16 个新鲜随机字节，随后返回 `Result<NodeId>`。它不读取数据库、不访问 RequestIdentity、不保存启动种子、时间戳或计数器，也不提供固定种子选项。“独立”指不复用请求 ID 的派生输入或结果；两者使用同一个 libsodium 系统安全随机设施，并不创建两套随机源。

[内部格式化](internal/NodeUuid.cc) 使用 UUID v4 布局（字节从 0 开始）：

- 第 6 字节高四位设为 `0100`，保留低四位。
- 第 8 字节高两位设为 `10`，保留低六位。
- 其余 122 位保持随机输入，输出为小写 `8-4-4-4-12` UUID 分组。

这些固定布局位保证结果非 nil，即使内部格式化测试输入全零也不例外。格式依据 [RFC 9562 UUIDv4](https://www.rfc-editor.org/rfc/rfc9562.html#section-5.4)。NodeId 解析仍接受其他非 nil UUID 版本，生成器不修改 core 的接受规则。

格式化后若违反 NodeId 解析不变量，返回 `InternalError`。`randombytes_buf` 没有错误返回通道，底层随机源的库级致命失败不能转换为 Result；无不安全 RNG 回退，也不声称能从所有操作系统故障恢复。正常适配器路径使用固定大小栈缓冲区；底层随机调用的系统行为和时延不由适配器保证。

随机 UUID 不能保证绝对无碰撞。生成器不查询已有节点或自动重试；数据库约束和调用方错误处理仍负责持久化冲突。

## 初始化及失败边界

[内部初始化实现](internal/SodiumSupport.cc) 在工厂中调用 `sodium_init()`，接受首次成功的 0 和已初始化的 1，其余状态返回 `InternalError`。重复初始化的同步由 libsodium 提供，不安装全局随机回调、不缓存失败，不提供不安全随机源回退。工厂将对象分配的 `std::bad_alloc` 转为 `InternalError`，不会返回空的成功对象。

初始化可能等待系统随机源就绪，必须在启动阶段完成，不能在每次 digestBearer 调用中执行。初始化没有截止时间保证；底层库的致命终止不能转为 Result，也不能宣称任意操作系统故障都可恢复。相关 API 语义见 [libsodium 初始化说明](https://doc.libsodium.org/usage)、[编码 API](https://doc.libsodium.org/helpers) 和 [SHA-2 API](https://doc.libsodium.org/advanced/sha-2_hash_function)。

[内部测试接缝](internal/SodiumSupport.h) 支持逐次调用的初始化／哈希函数，用于检查失败分类和校验顺序；它不是公开构造选项。生产工厂固定使用真实 sodium_init，生产 codec 固定使用 crypto_hash_sha256。测试不替换全局 RNG，不提供认证替代入口。

## 构建与测试

[CMakeLists.txt](CMakeLists.txt) 向 `anof_infrastructure` 注册实现，并私有链接根配置发现的 `PkgConfig::Sodium`。静态库消费者通过 CMake 目标获得最终链接所需依赖，公开头不要求包含 sodium 头。

`ANOF_BUILD_TESTS=ON` 时提供：

- `anof_node_id_generator_unittest`：[4 个 Boost.Test 案例](tests/NodeIdGenerator_unittest.cc)，CTest 超时 15 秒。覆盖确定性格式化、全零／全一／递增输入、256 种字节值的位保留检查、128 个生产样本的解析／非 nil／v4 布局，以及重复初始化构造。样本无重复不证明无碰撞。固定字节仅进入[内部格式化函数](internal/NodeUuid.h)，不替换 RNG。
- `anof_crypto_headers_check`：三个公开头分别独立编译，仅依赖 `anof_application`。
- `anof_request_identity_unittest`：[7 个 Boost.Test 案例](tests/RequestIdentity_unittest.cc)，CTest 超时 15 秒。包含三组固定种子下的 18 个独立已知答案、66 字节精确布局、编号边界、摘要截断与全零修正、版本／变体位保留、哈希失败无替代 ID、生产 provider 重复调用与独立构造。固定种子只传入[内部纯派生接缝](internal/RequestIdDerivation.h)，不生成生产 provider。
- `anof_session_token_codec_unittest`：[8 个 Boost.Test 案例](tests/SessionTokenCodec_unittest.cc)，CTest 超时 15 秒。覆盖独立固定 SHA-256 答案、规范编码往返、非法长度／字母表／padding／空白／尾随数据、末尾未使用位、无 NUL 结尾视图、重复构造和内部失败分类。

从项目根目录运行：

```sh
cmake --build cmake-build-debug --target anof_session_token_codec_unittest anof_request_identity_unittest anof_node_id_generator_unittest anof_crypto_headers_check --parallel 2
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_(session_token_codec|request_identity|node_id_generator)_unittest$' -j 1
```

Release 使用同名目标及 `cmake-build-release` 目录，两者均需开启 `ANOF_BUILD_TESTS`。测试只处理内存中的小数据，不运行随机源压力测试。内部错误注入不触发真实熵源失效、系统内存耗尽或进程终止；局部测试不验证 HTTP、真实 Session 鉴权、SQL 事务或完整实例关闭。
