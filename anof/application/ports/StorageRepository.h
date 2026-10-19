#pragma once
#include <anof/domain/transfer/Storage.h>

namespace anof {
class StorageReader {
public:
    virtual ~StorageReader() = default;
    virtual Result<StorageSnapshot> get() = 0;
};
class StorageRepository : public StorageReader {
public:
    // 必须与 Upload/逻辑配额/审计处于同一事务；语义与领域纯计算一致。
    virtual Result<StorageSnapshot> setBudget(Revision expected, ByteCount) = 0;
    virtual Result<void> reserve(ByteCount) = 0;
    virtual Result<void> releaseReservation(ByteCount) = 0;
    virtual Result<void> publishReservation(ByteCount) = 0;
};
}  // namespace anof
