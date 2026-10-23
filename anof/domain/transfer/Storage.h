#pragma once
#include <anof/core/Quantity.h>

namespace anof {
struct StorageSnapshot {
    ByteCount budget;
    ByteCount retained;
    ByteCount reserved;
    Revision revision;
};
// 输入可暂时超预算（允许读取/清理），但总量必须可表示；新预留不允许超过预算。
Result<StorageSnapshot> releaseRetainedStorage(StorageSnapshot, ByteCount);
Result<StorageSnapshot> setStorageBudget(StorageSnapshot, Revision expected, ByteCount);
Result<StorageSnapshot> reserveStorage(StorageSnapshot, ByteCount);
Result<StorageSnapshot> releaseStorage(StorageSnapshot, ByteCount);
Result<StorageSnapshot> publishStorage(StorageSnapshot, ByteCount);
}  // namespace anof
