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
    // 唯一ID插入有引用的AVAILABLE记录，真实事件时间不可省略；同事务创建Node并转换计量。
    // 不允许覆盖描述、无引用初始记录或任意删除。
    virtual Result<void> insert(const BlobRecord&) = 0;
};
}  // namespace anof
