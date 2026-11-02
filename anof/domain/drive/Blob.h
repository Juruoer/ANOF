#pragma once
#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/domain/transfer/TransferValues.h>

#include <optional>

namespace anof {
// 不存储任意路径，固定 key 只能由服务生成的 BlobId 推导。
struct Blob {
    BlobId id;
    FileLength size;
    Sha256 sha256;
    std::string storageKey() const {
        auto text = id.toString();
        return "objects/" + text.substr(0, 2) + "/" + text;
    }
};
// 内容描述与可变生命周期分离；下载只携带不可变描述，仓储持有完整记录。
enum class BlobState { Available = 0,
                       Deleting = 1,
                       Unavailable = 2 };
// 文件事实不暴露fd/POSIX结构；长度允许大于产品文件上限，以记录异常增长。
struct FileEvidence {
    std::uint64_t device, inode;
    ByteCount size;
    std::int64_t mtimeNs, ctimeNs;
};
enum class UnavailableReason { Missing,
                               Length,
                               Hash };
struct UnavailableEvidence {
    UnavailableReason reason;
    std::optional<FileEvidence> file;
    std::optional<Sha256> sha256;
};
Result<void> validateUnavailableEvidence(const UnavailableEvidence&, const Blob&);
Result<ByteCount> unavailableCharge(const Blob&, const UnavailableEvidence&);
struct BlobRecord {
    Blob content;
    BlobState state;
    UtcTime createdAt;
    UtcTime updatedAt;
    Revision revision;
    std::optional<UtcTime> unreferencedAt;
    std::optional<UtcTime> deleteStartedAt;
    ByteCount chargedBytes;
    std::optional<UnavailableEvidence> unavailable;
};
struct BlobGcPosition {
    UtcTime unreferencedAt;
    BlobId id;
};
inline constexpr std::int64_t kBlobGraceSeconds = 86400;
Result<void> validateBlobRecord(const BlobRecord&);
Result<UtcTime> blobGcDeadline(UtcTime unreferencedAt);
bool blobGcEligible(const BlobRecord&, UtcTime now, bool nodeReference, bool uploadReference) noexcept;
}  // namespace anof
