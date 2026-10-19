#pragma once
#include <anof/core/Id.h>
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
}  // namespace anof
