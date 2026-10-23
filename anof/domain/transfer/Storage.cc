#include <anof/domain/transfer/Storage.h>
namespace anof {
namespace {
Result<StorageSnapshot> advance(StorageSnapshot s) {
    auto revision = s.revision.add(1);
    if (!revision) return revision.error();
    s.revision = revision.value();
    return s;
}
}
Result<StorageSnapshot> setStorageBudget(StorageSnapshot s, Revision expected, ByteCount budget) {
    if (s.revision != expected) return ErrorCode::PreconditionFailed;
    auto total = s.retained.add(s.reserved.value());
    if (!total) return total.error();
    if (budget.value() < total.value().value()) return ErrorCode::StorageBudgetExceeded;
    if (budget == s.budget) return s;
    s.budget = budget;
    return advance(s);
}
Result<StorageSnapshot> reserveStorage(StorageSnapshot s, ByteCount amount) {
    auto total = s.retained.add(s.reserved.value());
    if (!total) return total.error();
    if (total.value().value() > s.budget.value() || amount.value() > s.budget.value() - total.value().value())
        return ErrorCode::StorageBudgetExceeded;
    s.reserved = s.reserved.add(amount.value()).value();
    return amount.value() == 0 ? Result<StorageSnapshot>(s) : advance(s);
}
Result<StorageSnapshot> releaseStorage(StorageSnapshot s, ByteCount amount) {
    if (!s.retained.add(s.reserved.value()) || amount.value() > s.reserved.value()) return ErrorCode::IntegrityError;
    s.reserved = ByteCount::from(s.reserved.value() - amount.value()).value();
    return amount.value() == 0 ? Result<StorageSnapshot>(s) : advance(s);
}
Result<StorageSnapshot> publishStorage(StorageSnapshot s, ByteCount amount) {
    if (!s.retained.add(s.reserved.value()) || amount.value() > s.reserved.value()) return ErrorCode::IntegrityError;
    s.retained = s.retained.add(amount.value()).value();
    s.reserved = ByteCount::from(s.reserved.value() - amount.value()).value();
    return amount.value() == 0 ? Result<StorageSnapshot>(s) : advance(s);
}
Result<StorageSnapshot> releaseRetainedStorage(StorageSnapshot s, ByteCount amount) {
    if (!s.retained.add(s.reserved.value()) || amount.value() > s.retained.value()) return ErrorCode::IntegrityError;
    s.retained = ByteCount::from(s.retained.value() - amount.value()).value();
    return amount.value() == 0 ? Result<StorageSnapshot>(s) : advance(s);
}
}  // namespace anof
