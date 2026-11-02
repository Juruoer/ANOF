#pragma once
#include <anof/application/ports/OfflineInstanceAccess.h>

namespace anof {
struct MaintenanceFileObservation {
    MaintenanceFileKey key;
    FileEvidence evidence;
    std::optional<Sha256> sha256;
};
// 异常目录项不能强制转成合法key；报告类型化unsafe finding，不打开异常路径。
using InventoryItem = std::variant<MaintenanceFileObservation, MaintenanceFinding>;
// next使用创建时operationId和本次取消标志，阶段为Inspect；入口拒绝不消费枚举位置。
class MaintenanceInventory {
public:
    virtual ~MaintenanceInventory() = default;
    virtual MaintenanceResult<std::vector<InventoryItem>> next(const MaintenanceContext&, PageSize) = 0;  // 空页表示结束；有界磁盘索引可用。
};
// 已校验目录类型/manifest/COMPLETE，reader仍须逐页/逐对象验证。不能仅信完成标记。
// objects绑定openBackup时operationId，阶段为Verify；manifest仅访问自有缓存，不执行I/O或取消检查。
class MaintenanceBackupSource {
public:
    virtual ~MaintenanceBackupSource() = default;
    virtual const BackupManifest& manifest() const noexcept = 0;
    virtual MaintenanceResult<Page<Blob, BlobId>> objects(const MaintenanceContext&, PageSize, std::optional<BlobId>) = 0;
};
class MaintenanceFileStore {
public:
    virtual ~MaintenanceFileStore() = default;
    // 在已提交意图之后、元数据事务之外，同一lease内重新取证并完成动作所需文件/目录sync。
    // binding由可信编排传入，适配器核对动作特定事实；不能把输入摘要直接回显为成功。
    // 失败/未知返回MaintenanceFailure，不能签发Durable。重启须重新调用，不能复用序列化旧证据。
    // 纯RecomputeAccounting不调用本端口，元数据适配器在事务内重算并核对proof。
    virtual MaintenanceResult<MaintenanceActionEvidence> verifyAction(const MaintenanceContext&,
                                                                      const InstanceLease&, const MaintenanceEvidenceBinding&, const MaintenanceAction&) = 0;
    // reader在lease前销毁；不递归跟随链接，不把全部目录项装进内存。
    virtual MaintenanceResult<std::unique_ptr<MaintenanceInventory>> inventory(
        const MaintenanceContext&, const InstanceLease&, InspectionMode) = 0;
    // null只表示在安全父目录下确认缺失；类型/权限/换inode是错误，不等价缺失。
    virtual MaintenanceResult<std::optional<MaintenanceFileObservation>> observe(
        const MaintenanceContext&, const InstanceLease&, const MaintenanceFileKey&, InspectionMode) = 0;
    // 64KiB流式复制并核对完整hash，前后证据一致；禁止覆盖、链接、跨源目录逃逸。
    virtual MaintenanceResult<void> copyObject(const MaintenanceContext&, const InstanceLease&,
                                               const MaintenanceDestination&, const Blob&, const FileEvidence&) = 0;
    // 原数据库/sidecar/config/format逐字保全，清单+完成标记同步并不覆盖发布后才成功。
    virtual MaintenanceResult<void> preserveOriginal(const MaintenanceContext&, const InstanceLease&,
                                                     const MaintenanceDestination&, const MaintenanceFingerprint&) = 0;
    // 重入前验证既有原始副本的完成标记、源绑定和全部文件摘要，不打开其中的SQLite。
    virtual MaintenanceResult<void> verifyOriginal(const MaintenanceContext&, const std::filesystem::path&, const MaintenanceFingerprint&) = 0;
    // 已提交PREPARED，复验两位置和hash；rename-noreplace及两目录fsync，未知返回PublicationOutcomeUnknown。
    virtual MaintenanceResult<FileEvidence> quarantine(const MaintenanceContext&, const InstanceLease&,
                                                       const QuarantineEntry&) = 0;
    virtual MaintenanceResult<void> truncateTail(const MaintenanceContext&, const InstanceLease&,
                                                 const TruncateUnconfirmedTail&) = 0;
    virtual MaintenanceResult<std::unique_ptr<MaintenanceBackupSource>> openBackup(
        const MaintenanceContext&, const std::filesystem::path&) = 0;
    // 副本数据库/config和对象均流式验证大小/hash，拒绝未列出文件；不继承备份的运行配置。
    // 向source.objects转发本次context；source须以本次operationId打开，跨操作应重新openBackup。
    virtual MaintenanceResult<void> copyBackup(const MaintenanceContext&, MaintenanceBackupSource&,
                                               const MaintenanceDestination&) = 0;
    virtual MaintenanceResult<BackupFileDigest> writeConfiguration(const MaintenanceContext&,
                                                                   const MaintenanceDestination&, const InstanceConfiguration&) = 0;
    // 有界页，严格递增ID，写入本运行的index；finish时返回实际size/hash/count/bytes。
    virtual MaintenanceResult<void> appendObjectIndex(const MaintenanceContext&,
                                                      const MaintenanceDestination&, const std::vector<Blob>& page) = 0;
    virtual MaintenanceResult<BackupObjectIndex> finishObjectIndex(const MaintenanceContext&,
                                                                   const MaintenanceDestination&) = 0;
    virtual MaintenanceResult<void> sealBackup(const MaintenanceContext&,
                                               const MaintenanceDestination&, const BackupManifest&) = 0;
    // 写入format及恢复operationId，不包含旧锁/sidecar；完整验证后方可publish。
    virtual MaintenanceResult<void> sealRestoredInstance(const MaintenanceContext&,
                                                         const MaintenanceDestination&, InstanceId) = 0;
    // 所有文件/index/manifest/COMPLETE已核对并sync；同父不覆盖rename+父fsync。
    // rename后父sync未知必须保留目标与operationId，不能返回普通IoError或重试覆盖。
    virtual MaintenanceResult<void> publish(const MaintenanceContext&, const MaintenanceDestination&) = 0;
};
}  // namespace anof
