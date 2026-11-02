#pragma once

#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/core/Page.h>
#include <anof/domain/drive/Blob.h>

#include <variant>

namespace anof {
using MaintenanceResourceId = Id<struct MaintenanceResourceIdTag>;
using MaintenanceRunId = Id<struct MaintenanceRunIdTag>;
using MaintenancePlanId = Id<struct MaintenancePlanIdTag>;
using MaintenanceActionId = Id<struct MaintenanceActionIdTag>;
using QuarantineId = Id<struct QuarantineIdTag>;
using BackupId = Id<struct BackupIdTag>;
inline constexpr int kMaintenancePlanFormat = 1, kMaintenanceReportFormat = 1, kBackupFormat = 1;
inline constexpr std::size_t kMaintenanceBlockBytes = 65536, kMaintenanceFindingBytes = 4096,
                             kMaintenanceFindingLimit = 10000, kMaintenanceReportBytes = 48 * 1024 * 1024,
                             kMaintenanceActionLimit = 10000, kMaintenancePlanBytes = 16 * 1024 * 1024,
                             kMaintenanceJsonDepth = 16, kBackupManifestBytes = 65536,
                             kBackupObjectLineBytes = 512, kBackupObjectLimit = 100000;
using MaintenanceActionIndex = Quantity<struct MaintenanceActionIndexTag, 0, 9999>;
inline constexpr std::int64_t kCompletedMaintenanceRetentionSeconds = 90 * 86400;
using AuditRetentionDays = Quantity<struct AuditRetentionDaysTag, 7, 3650>;
using AuditBatchSize = Quantity<struct AuditBatchSizeTag, 1, 1000>;
enum class FindingCode {
    DatabaseCorrupt,
    ForeignKeyBroken,
    TreeInvalid,
    AccountingMismatch,
    UploadStateInconsistent,
    ObjectMissing,
    ObjectLengthMismatch,
    ObjectHashMismatch,
    OrphanObject,
    UnsafeEntry,
    QuarantinePresent,
    MaintenancePending,
    CheckLimitExceeded
};
enum class FindingSeverity { Info,
                             Warning,
                             Error };
enum class MaintenanceReportStatus { Healthy,
                                     Findings,
                                     Incomplete };
enum class InspectionMode { Quick,
                            Full };
enum class MaintenanceResource { Instance,
                                 Database,
                                 User,
                                 Space,
                                 Node,
                                 Blob,
                                 Upload,
                                 Quarantine,
                                 Run };
// 不接受路径字符串；即使是孤儿文件，也只能使用合法的受检ID推导键。
using MaintenanceFileKey = std::variant<BlobId, UploadId, QuarantineId>;
std::string maintenanceStorageKey(const MaintenanceFileKey&);
struct MaintenanceFinding {
    FindingCode code;
    FindingSeverity severity;
    MaintenanceResource resource;
    std::optional<MaintenanceResourceId> resourceId;  // 不能转用作文件路径。
    std::optional<MaintenanceFileKey> file;
    bool repairable = false;
};
struct MaintenanceReport {
    ByteCount info, warnings, errors;
    bool complete = true;
    bool detailsTruncated = false;
    MaintenanceReportStatus status() const noexcept;
};
// 原始数据库/sidecar/config/format摘要与有序文件证据摘要分别绑定；绝非SQL文本。
struct MaintenanceFingerprint {
    Sha256 metadata;
    Sha256 inventory;
};
struct RecomputeAccounting {
    Sha256 provenLedger;
};
struct TruncateUnconfirmedTail {
    UploadId id;
    UploadGeneration generation;
    UploadOffset confirmed;
    FileEvidence before;
};
struct ResumeUpload {
    UploadId id;
    UploadGeneration generation;
    Sha256 responsibility;
};
struct ResumeDeletingBlob {
    BlobId id;
    Revision revision;
    Sha256 responsibility;
};
struct QuarantineOrphan {
    std::variant<BlobId, UploadId> source;
    QuarantineId destination;
    FileEvidence before;
    Sha256 sha256;
};
struct MarkBlobUnavailable {
    BlobId id;
    Revision revision;
    UnavailableEvidence evidence;
};
struct VerifyRestoredBlob {
    BlobId id;
    Revision revision;
    FileEvidence observed;
    Sha256 sha256;
};
using MaintenanceActionPayload = std::variant<RecomputeAccounting, TruncateUnconfirmedTail, ResumeUpload,
                                              ResumeDeletingBlob, QuarantineOrphan, MarkBlobUnavailable, VerifyRestoredBlob>;
struct MaintenanceAction {
    MaintenanceActionId id;
    MaintenanceActionPayload payload;
    Sha256 expectedPostcondition;  // 确定性编码的typed后置事实摘要，由执行器重新生成验证。
};
struct MaintenancePlan {
    MaintenancePlanId id;
    InstanceId instance;
    std::int64_t schemaVersion = 5, formatVersion = 5;
    int planFormat = kMaintenancePlanFormat;
    MaintenanceFingerprint source;
    InspectionMode mode;
    MaintenanceReport inspection;
    std::vector<MaintenanceAction> actions;
    // 排除计划允许变化的行/文件后的现场摘要，用于重入检测无关修改。
    std::optional<MaintenanceFingerprint> continuation = {};
};
Result<void> validateMaintenancePlan(const MaintenancePlan&);
enum class MaintenanceActionState { Prepared,
                                    Applied,
                                    Verified };
// 进程内可信适配器返回的事实，不是用户plan/JSON输入，也不是密码学能力令牌。
struct MaintenanceEvidenceBinding {
    InstanceId instance;
    MaintenanceRunId run;
    MaintenancePlanId plan;
    Sha256 planDigest;
    MaintenanceActionId action;
    Revision actionRevision;
};
struct MaintenanceFileFact {
    MaintenanceFileKey key;
    std::optional<FileEvidence> file;  // null表示安全父目录下确认缺失，不代表读取失败。
    std::optional<Sha256> sha256;
};
enum class MaintenanceEvidenceQuality { Observed,
                                        Durable,
                                        Unknown };
struct MaintenanceActionEvidence {
    MaintenanceEvidenceBinding binding;
    MaintenanceEvidenceQuality quality;
    std::vector<MaintenanceFileFact> files;  // <=2，所有动作的局部文件事实；不是完整实例清单。
    Sha256 postcondition;
};
// 仅检查绑定与结构；具体动作的预期文件/长度/hash/元数据事实由适配器复验。
Result<void> validateMaintenanceEvidence(const MaintenanceEvidenceBinding&, const MaintenanceAction&,
                                         const MaintenanceActionEvidence&);
struct MaintenanceActionRecord {
    MaintenanceActionIndex index;
    MaintenanceAction action;
    MaintenanceActionState state;
    std::optional<MaintenanceActionEvidence> resultEvidence;
    UtcTime createdAt, updatedAt;
    Revision revision;
};
struct MaintenanceRun {
    MaintenanceRunId id;
    MaintenancePlanId plan;
    InstanceId instance;
    Sha256 planDigest;
    UtcTime createdAt, updatedAt;
    bool completed = false;
    Revision revision;
};
enum class QuarantineState { Prepared,
                             Held };
struct QuarantineEntry {
    QuarantineId id;
    std::variant<BlobId, UploadId> source;
    FileEvidence evidence;
    Sha256 sha256;
    ByteCount chargedBytes;
    QuarantineState state;
    MaintenancePlanId plan;
    MaintenanceActionId action;
    UtcTime createdAt, updatedAt;
    Revision revision;
};
Result<void> validateQuarantineEntry(const QuarantineEntry&);
Result<void> validateMaintenanceActionRecord(const MaintenanceActionRecord&);
struct BackupFileDigest {
    ByteCount size;
    Sha256 sha256;
};
struct BackupObjectIndex {
    BackupFileDigest file;
    NodeCount count;
    ByteCount totalBytes;
};
struct BackupExclusions {
    NodeCount quarantineObjects;
    ByteCount quarantineBytes;
    NodeCount unreferencedObjects;
    ByteCount unreferencedBytes;
    NodeCount uploads;
};
struct BackupManifest {
    int backupFormat = kBackupFormat;
    std::string backend = "sqlite";
    std::string applicationVersion;
    std::int64_t schemaVersion = 5, formatVersion = 5;
    BackupId id;
    InstanceId instance;
    UtcTime createdAt;
    BackupFileDigest database, configuration;
    BackupObjectIndex objects;
    BackupExclusions exclusions;
};
Result<void> validateBackupManifest(const BackupManifest&);
// 分页索引验证持有常量内存；调用方还必须校验编码、字节摘要及最终文件集合。
class BackupIndexValidator {
public:
    Result<void> append(const Blob&);
    Result<void> finish(const BackupObjectIndex&) const;

private:
    std::optional<BlobId> last_;
    NodeCount count_;
    ByteCount bytes_;
};
}  // namespace anof
