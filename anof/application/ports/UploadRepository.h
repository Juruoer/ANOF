#pragma once
#include <anof/core/Page.h>
#include <anof/domain/transfer/TransferPolicy.h>

namespace anof {
enum class UploadScan { Active,
                        CleanupPending,
                        Expired,
                        RetentionElapsed };
class UploadReader {
public:
    virtual ~UploadReader() = default;
    virtual Result<std::optional<Upload>> find(UploadId) = 0;
    virtual Result<std::optional<Upload>> findByClientId(UserId, OperationId) = 0;
    virtual Result<UploadCounts> counts(UserId) = 0;
    virtual Result<Page<Upload, UploadPosition>> list(UserId, PageSize, std::optional<UploadPosition>) = 0;
    // 全局按 created_at/id seek；Expired 排除 COMMITTING；RetentionElapsed 排除 cleanup_pending。
    virtual Result<Page<Upload, UploadPosition>> scan(UploadScan, UtcTime now, PageSize,
                                                      std::optional<UploadPosition>) = 0;
    // 活跃和终态 cleanup_pending 的 length-offset 总和，溢出为 IntegrityError。
    virtual Result<ByteCount> unwrittenCommitment() = 0;
};
class UploadRepository : public UploadReader {
public:
    // 原子检查两类计数上限及 user/client 唯一性；只接纳有效 CREATING generation=1。
    virtual Result<void> insert(const Upload&) = 0;
    // 同一事务读取旧值，validateUploadReplacement，按 state/generation/offset 比较更新。
    // 条件不符 Conflict；不存在 NotFound。禁止无条件覆盖；调用者负责授权与相关账本操作。
    virtual Result<void> replace(UploadId, UploadMatch expected, const Upload& replacement) = 0;
    // 用户禁用的同一事务：全部非终态标记不可逆取消并递增 generation（已标记者不变）。
    // 最多64条；任一溢出整个操作失败，由事务回滚，不留下部分成功。
    virtual Result<std::vector<UploadId>> requestUserCancellation(UserId) = 0;
    // 仅在 mayForgetUpload 且 generation 匹配时删除；不删除文件或扣账。
    virtual Result<void> eraseTerminal(UploadId, UploadGeneration expected, UtcTime now) = 0;
};
}  // namespace anof
