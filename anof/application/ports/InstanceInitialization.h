#pragma once

#include <anof/application/maintenance/TransferInitialization.h>

namespace anof {
class InitializationLease {
public:
    InitializationLease() = default;
    InitializationLease(const InitializationLease&) = delete;
    InitializationLease& operator=(const InitializationLease&) = delete;
    virtual ~InitializationLease() = default;
    // 析构只释放同一实例锁和描述符，不清空半成品；借用不得超过本对象寿命。
    virtual const InstanceLease& lease() const = 0;
    virtual const InitializationIdentity& identity() const = 0;
    // 必须持久化 diskSafety；配置中不保存 contentBudget。
    virtual InitResult<void> writeConfiguration(const InstanceConfiguration&) = 0;
    // 不覆盖既有标记；finish 遇到完全匹配的已发布标记只验证，之后清理本次 pending。
    virtual InitResult<void> publishFormat() = 0;
    virtual InitResult<void> clearPending() = 0;
};
class InstanceInitializer {
public:
    virtual ~InstanceInitializer() = default;
    // 原子创建 .anof、稳定锁及持久化 pending；既有目录明确拒绝，不另建替换锁。
    virtual InitResult<std::unique_ptr<InitializationLease>> create(const std::filesystem::path&,
                                                                    const InitializationIdentity&) = 0;
    virtual InitResult<std::unique_ptr<InitializationLease>> openPending(const std::filesystem::path&) = 0;
};
class InstanceBootstrap {
public:
    virtual ~InstanceBootstrap() = default;
    // 仅接受不存在的主库；schema/format均为4。以initialNodeAccounting(seed.identity)初始化节点账本，
    // 与根Node/额度/用户/审计同事务；create/verify检查真实行数，不补账或支持开发版本升级。
    virtual InitResult<InitReceipt> createCurrent(const InstanceLease&, const TransferBootstrapSeed&) = 0;
    // 检查已提交引导及 ID 一致性；允许 pending，不重新建用户、重设密码或补数据。
    virtual InitResult<InitReceipt> verifyComplete(const InstanceLease&, const InitializationIdentity&) = 0;
};
}  // namespace anof
