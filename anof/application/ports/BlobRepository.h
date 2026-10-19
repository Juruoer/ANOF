#pragma once
#include <anof/core/Page.h>
#include <anof/domain/drive/Blob.h>
#include <optional>

namespace anof {
class BlobReader {
public:
    virtual ~BlobReader() = default;
    virtual Result<std::optional<Blob>> find(BlobId) = 0;
    virtual Result<std::optional<Blob>> findByNode(NodeId) = 0;
    // READY/COMMITTING 仅表示固定候选ID，尚无 Blob 行也须可查询。
    virtual Result<std::optional<BlobId>> candidateForUpload(UploadId) = 0;
    virtual Result<bool> hasNodeReference(BlobId) = 0;
    virtual Result<bool> hasUploadReference(BlobId) = 0;
    virtual Result<Page<Blob, BlobId>> list(PageSize, std::optional<BlobId>) = 0;
};
class BlobRepository : public BlobReader {
public:
    // 唯一ID插入 AVAILABLE；不允许覆盖描述或任意删除。
    virtual Result<void> insert(const Blob&) = 0;
};
}  // namespace anof
