#pragma once
#include <anof/core/Id.h>
#include <anof/core/Clock.h>
#include <optional>
#include <anof/domain/transfer/TransferValues.h>

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
enum class BlobState { Available, Deleting };
struct BlobRecord {
    Blob content;
    BlobState state;
    UtcTime createdAt;
    UtcTime updatedAt;
    Revision revision;
    std::optional<UtcTime> unreferencedAt;
    std::optional<UtcTime> deleteStartedAt;
};
struct BlobGcPosition { UtcTime unreferencedAt; BlobId id; };
inline constexpr std::int64_t kBlobGraceSeconds = 86400;
Result<void> validateBlobRecord(const BlobRecord&);
Result<UtcTime> blobGcDeadline(UtcTime unreferencedAt);
bool blobGcEligible(const BlobRecord&, UtcTime now, bool nodeReference, bool uploadReference) noexcept;
}  // namespace anof
