#pragma once
#include <anof/core/Id.h>
namespace anof {
class TransferIdGenerator {
public:
    virtual ~TransferIdGenerator() = default;
    virtual Result<UploadId> uploadId() = 0;
    virtual Result<BlobId> blobId() = 0;
};
}  // namespace anof
