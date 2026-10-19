# Infrastructure 模块

本模块提供 application 端口的具体适配器。调用方使用业务端口，实例装配代码通过下列工厂创建对象；适配器不依赖 HTTP 或 runtime 类型。

| 范围 | 构造入口 | 参考文档 |
|---|---|---|
| SQLite 传输元数据 | `sqlite::openSqliteTransferMetadataStore(path, options)` | [共享传输事务与仓储](persistence/sqlite/README.md) |
| SQLite 元数据 | `sqlite::openSqliteMetadataStore(path, options)` | [持久化接口、当前 schema 及事务生命周期](persistence/sqlite/README.md) |
| 初始化文件与 lease | `system::makeLinuxInstanceInitializer()` | [文件协议与安全发布](system/README.md) |
| SQLite 事务化引导 | `sqlite::makeSqliteInstanceBootstrap(options)` | [引导与收尾验证](persistence/sqlite/README.md) |
| 实例格式与数据库维护 | `sqlite::makeSqliteInstanceMaintenance(options)` | [维护校验与资源生命周期](persistence/sqlite/README.md) |
| 密码哈希／验证 | `crypto::makeSodiumPasswordHasher(policy)` | [显式 Argon2id 预算](crypto/README.md) |
| 随机凭据签发 | `crypto::makeSodiumCredentialIssuer()` | [凭据编码与独立摘要类型](crypto/README.md) |
| 邀请 token 摘要 | `crypto::makeSodiumInviteTokenCodec()` | [规范邀请编码](crypto/README.md) |
| 身份实体 ID | `crypto::makeSodiumIdentityIdGenerator()` | [类型化 UUID 生成](crypto/README.md) |
| Bearer token 摘要 | `crypto::makeSodiumSessionTokenCodec()` | [crypto 接口与错误语义](crypto/README.md) |
| 请求关联 ID | `crypto::makeSodiumRequestIdentity()` | [启动种子与派生规则](crypto/README.md) |
| 传输实体 ID | `crypto::makeSodiumTransferIdGenerator()` | [Upload／Blob UUID](crypto/README.md) |
| 本地内容文件与哈希 | `storage::openLocalTransferFileStore(root)` | [文件访问与同步边界](storage/local/README.md) |
| 随机节点 ID | `crypto::makeSodiumNodeIdGenerator()` | [UUID v4 生成与位布局](crypto/README.md) |
| 实例 TOML 配置 | `config::makeTomlInstanceConfigurationLoader()` | [配置格式与有界读取](config/README.md) |
| Linux 实例排他锁 | `system::makeLinuxInstanceLockProvider()` | [路径检查、锁与 lease 生命周期](system/README.md) |
| UTC 系统时钟 | `system::makeSystemClock()` | [时间精度、取整与范围](system/README.md) |

上述命名空间均位于 `anof::infrastructure` 内，文件工厂返回 `FileResult<std::unique_ptr<TransferFileStore>>`，其余普通工厂返回 `Result<std::unique_ptr<对应端口>>`，初始化文件与 Bootstrap 工厂返回带阶段信息的 `InitResult<std::unique_ptr<对应端口>>`。成功后调用方接管独占所有权，失败时检查稳定错误分类；不得以空资源发布就绪状态。

## 构造与装配边界

所有表列工厂均已实现。密码适配器要求显式 SodiumPasswordPolicy；Store、Bootstrap 和启动维护分别要求 SqliteMetadataOptions、SqliteBootstrapOptions、SqliteMaintenanceOptions，其中 passwordHashLimits 应与密码验证接纳预算一致。没有可用的零初始化密码预算或旧接口兼容重载。

初始化文件协议、事务化引导和 M2 启动维护有真实局部组合测试。根 CLI／runtime 的初始化编排、密码 worker 调度与 HTTP 身份流程不由本模块提供。账户授权、HTTP 映射与远程命令由上层实现；模块测试仅验证适配器及其事务组合。

## 构建依赖

[CMakeLists.txt](CMakeLists.txt) 包含 persistence、crypto、system、config、storage/local。统一静态库目标为 `anof_infrastructure`，公开链接 `anof_application`，私有链接 `SQLite::SQLite3` 、`PkgConfig::Sodium`、`tomlplusplus::tomlplusplus` 和 `nlohmann_json::nlohmann_json`。根配置使用已安装的 SQLite 与 libsodium，通过 pkg-config 发现后者；没有运行时下载依赖。

消费者应链接 CMake 目标，不复制库文件或硬编码系统库路径。静态链接的最终可执行文件仍需解析 SQLite／libsodium，CMake 目标会传递所需链接依赖。公开工厂头不暴露第三方类型，具体头文件见对应参考文档。

## 所有权和运行约束

实例准备通过 [prepareInstance](../application/maintenance/PrepareInstance.h) 组合三个启动适配器：先获取真实 lease，再读取并复验配置，最后同步维护校验。失败释放 lease，成功把 lease 和自有配置交给 PreparedInstance。维护连接在返回前关闭；后续业务仓储由 metadata worker 自行构造。lease 须保持到其他实例资源清理结束，不能在启动准备完成或 runtime Stopped 时提前释放。

- 元数据 worker 内构造、使用并销毁 MetadataStore、Clock、NodeIdGenerator；实例需显式建库与引导，业务打开不得隐式更新 schema。
- SessionTokenCodec 与 RequestIdentity 在启动期间构造。每个实例复用一个 RequestIdentity；两者须存活到 HTTP 清理及迟到任务观测结束，销毁不能与调用并发。
- libsodium 初始化由 crypto 工厂完成，可能等待系统随机源；它不属于每次 token 校验或请求 ID 派生的路径。可报告的初始化／分配错误返回 InternalError，底层库致命终止不承诺可恢复。
- token 摘要不等于 Session 认证；RequestId 不用于授权、幂等或节点标识；UUID 生成不查询数据库；墙上时钟不是单调计时器。

## 验证入口

SQLite 测试覆盖结构校验、用户分页、私有空间定位、条件更新、只读快照、事务组合、额度边界及独立连接竞争。用户列表与私有空间查询通过读快照和写事务共用相同实现；会话撤销与身份审计由调用方显式组合，仓储不自行编排账户用例。

`ANOF_BUILD_TESTS=ON` 时提供 SQLite、crypto、system、config 和本地文件测试，均注册有限超时。其中包含引用根共享用例的 `anof_sqlite_contract_unittest`。模块头文件检查目标验证声明自包含；测试数量及实际运行结果以 CTest 为准。

从项目根目录构建模块回归与头文件检查，并执行模块测试：

```sh
cmake --build cmake-build-debug --parallel 2 --target \
    anof_infrastructure anof_sqlite_headers_check anof_sqlite_transfer_headers_check anof_crypto_headers_check \
    anof_system_headers_check anof_config_headers_check \
    anof_sqlite_foundation_headers_check anof_sqlite_identity_headers_check anof_m2_adapter_headers_check \
    anof_sqlite_foundation_unittest anof_sqlite_identity_unittest anof_sqlite_read_snapshot_unittest anof_sqlite_transaction_unittest anof_sqlite_node_unittest \
    anof_sqlite_contract_unittest anof_sqlite_quota_audit_unittest anof_sqlite_acceptance_unittest \
    anof_sqlite_m2_boundaries_unittest anof_sqlite_instance_bootstrap_unittest anof_sqlite_instance_maintenance_unittest anof_sqlite_instance_preparation_unittest \
    anof_sqlite_transfer_contract_unittest anof_sqlite_transfer_unittest anof_transfer_files_unittest \
    anof_session_token_codec_unittest anof_request_identity_unittest anof_node_id_generator_unittest \
    anof_password_hasher_unittest anof_m2_credentials_unittest \
    anof_system_clock_unittest anof_instance_lock_unittest anof_instance_initializer_unittest anof_instance_configuration_unittest
ctest --test-dir cmake-build-debug --output-on-failure -R '^anof_(sqlite_.*|transfer_files|session_token_codec|request_identity|node_id_generator|password_hasher|m2_credentials|system_clock|instance_lock|instance_initializer|instance_configuration)_unittest$' -j 1
```

另有 application 所属 `anof_prepare_instance_unittest`，覆盖编排端口及替身行为，不计入上述模块数量。Release 使用 `cmake-build-release`。测试目标名、构建命令和覆盖范围详见各子目录参考文档。局部测试与真实 SQLite 契约不等于真实 HTTP／runtime／crypto／SQLite 组合验证，也不证明硬件断电持久性或随机 ID 绝对无碰撞。

## 启动适配接口

[LinuxInstanceLock.h](system/LinuxInstanceLock.h)、[TomlInstanceConfiguration.h](config/TomlInstanceConfiguration.h) 和 [SqliteInstanceMaintenance.h](persistence/sqlite/SqliteInstanceMaintenance.h) 声明实例锁、配置加载及维护工厂。三个工厂均可构造对应端口。维护在 lease 持有期间同步验证实例格式、数据库版本及完整性，返回前关闭维护连接。模块测试覆盖真实锁、配置与维护，以及 prepareInstance 的真实适配组合和失败释放；不代表完整 serve／信号停止组合验收。文件表示、路径权限和错误分类约定见[实例启动契约](InstanceStartupContract.md)。

## 传输适配边界

生产建库、打开与初始化统一使用 schema／format 3；仅接纳当前格式，不升级或重置旧实例。传输仓储、文件节点、逻辑预留、物理计量与审计共用事务，类型化生产工厂直接返回 TransferMetadataStore。初始化接纳显式内容预算，持久化磁盘安全配置并创建三个内容目录。建库入口、共享事务和测试说明见 [SQLite 参考](persistence/sqlite/README.md)。本模块不提供上传用例、恢复编排、HTTP 流控或磁盘 worker。
