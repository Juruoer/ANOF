#include <anof/infrastructure/persistence/sqlite/SqliteDriveIntegrity.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {

SqliteTransactionContext::~SqliteTransactionContext() {
    if (!open_) return;
    if (readOnly_)
        (void)connection_.endRead();
    else
        (void)connection_.rollback();
    storeActive_ = false;
}

Result<void> SqliteTransactionContext::begin() {
    if (started_ || storeActive_) return ErrorCode::InvalidArgument;
    started_ = true;
    auto result = connection_.beginWrite();
    if (!result) return result.error();
    open_ = true;
    storeActive_ = true;
    return {};
}

Result<void> SqliteTransactionContext::beginRead() {
    if (started_ || storeActive_) return ErrorCode::InvalidArgument;
    started_ = true;
    auto result = connection_.beginRead();
    if (!result) return result.error();
    readOnly_ = true;
    open_ = true;
    storeActive_ = true;
    return {};
}

Result<void> SqliteTransactionContext::requireOpen() {
    if (!open_) return ErrorCode::InvalidArgument;
    if (failed_ || !connection_.usable() || !connection_.inTransaction()) {
        failed_ = true;
        return ErrorCode::InternalError;
    }
    return {};
}

Error SqliteTransactionContext::fail(Error error) {
    if (open_) failed_ = true;
    return error;
}

Result<void> SqliteTransactionContext::commit(const std::function<Result<void>()>& beforeCommit) {
    if (!open_ || readOnly_) return ErrorCode::InvalidArgument;
    auto ready = requireOpen();
    if (ready) ready = verifyDriveMembership(connection_);
    if (beforeCommit) {
        auto allowed = beforeCommit();
        if (ready && !allowed) ready = allowed.error();
    }
    // 关闭标志先独立于底层连接，保证旧事务析构不会回滚后继事务。
    open_ = false;
    storeActive_ = false;
    if (!ready) {
        auto rollback = connection_.rollback();
        if (!rollback) return rollback.error();
        return ready.error();
    }
    return connection_.commit();
}

}  // namespace anof::infrastructure::sqlite
