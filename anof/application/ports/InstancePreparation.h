#pragma once

#include <anof/core/Result.h>
#include <anof/domain/transfer/TransferValues.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace anof {

// 仅包含当前启动链路使用的配置；不引入 TOML、HTTP 或数据库实现类型。
struct InstanceConfiguration {
    std::string listenAddress = "127.0.0.1";
    std::uint16_t port = 8080;
    bool developmentHttp = false;
    std::size_t metadataQueue = 256;
    std::size_t httpInFlight = 128;
    double shutdownGraceSeconds = 30;
    DiskSafetyPolicy diskSafety;
};

// 成功取得后独占整个实例，析构释放；不得删除锁文件或以 PID 文件代替内核锁。
// root 是已校验的绝对目录，包含 .anof；持有者保证期间目录不被服务账户之外的主体替换。
class InstanceLease {
public:
    virtual ~InstanceLease() = default;
    virtual const std::filesystem::path& root() const noexcept = 0;
};

class InstanceLockProvider {
public:
    virtual ~InstanceLockProvider() = default;
    // 非阻塞竞争；已占用返回 Conflict。不得创建缺失实例或返回空的成功值。
    virtual Result<std::unique_ptr<InstanceLease>> acquire(const std::filesystem::path& root) = 0;
};

class InstanceConfigurationLoader {
public:
    virtual ~InstanceConfigurationLoader() = default;
    // 仅在持锁期间读取；实现负责文件类型、权限、有界读取及语法检查。
    virtual Result<InstanceConfiguration> load(const InstanceLease& lease) = 0;
};

class InstanceMaintenance {
public:
    virtual ~InstanceMaintenance() = default;
    // 监听前的同步维护阶段。核对已有实例格式及元数据版本／完整性，并在返回前关闭维护连接。
    // 不创建实例、账户或默认凭据，不静默升级／降级数据库；升级需要另行显式维护入口。
    virtual Result<void> verify(const InstanceLease& lease) = 0;
};

}  // namespace anof
