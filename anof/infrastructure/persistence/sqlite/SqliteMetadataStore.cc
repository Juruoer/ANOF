#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInitializedState.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataReadSnapshot.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStoreInternal.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>

#include <new>

namespace anof::infrastructure::sqlite {
Result<std::unique_ptr<MetadataTransaction>> detail::MetadataStoreImpl::beginWrite() {
    if (active_) return ErrorCode::InvalidArgument;
    try {
        auto transaction = std::make_unique<SqliteMetadataTransaction>(*connection_, active_, limits_);
        auto begun = transaction->begin();
        if (!begun) return begun.error();
        return std::unique_ptr<MetadataTransaction>(std::move(transaction));
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}
Result<std::unique_ptr<MetadataReadSnapshot>> detail::MetadataStoreImpl::beginRead() {
    if (active_) return ErrorCode::InvalidArgument;
    try {
        return makeSqliteReadSnapshot(*connection_, active_, limits_);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}

Result<std::unique_ptr<TransferTransaction>> detail::MetadataStoreImpl::beginTransferWrite() {
    if (active_) return ErrorCode::InvalidArgument;
    try {
        auto tx = std::make_unique<SqliteMetadataTransaction>(*connection_, active_, limits_);
        auto begun = tx->begin();
        if (!begun) return begun.error();
        return std::unique_ptr<TransferTransaction>(std::move(tx));
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}
Result<std::unique_ptr<TransferReadSnapshot>> detail::MetadataStoreImpl::beginTransferRead() {
    if (active_) return ErrorCode::InvalidArgument;
    try {
        return makeSqliteTransferReadSnapshot(*connection_, active_, limits_);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}

Result<std::unique_ptr<TransferMetadataStore>> openSqliteTransferMetadataStore(const std::filesystem::path& path, SqliteMetadataOptions options) {
    const auto limits = options.passwordHashLimits;
    if (!detail::validPasswordLimits(limits)) return ErrorCode::InvalidArgument;
    try {
        auto connection = openSqliteDatabase(path, options.connection);
        if (!connection) return connection.error();
        auto valid = detail::verifyInitialized(*connection.value(), limits);
        if (!valid) return valid.error().code == ErrorCode::NotFound ? Error{ ErrorCode::IntegrityError } : valid.error();
        if (!connection.value()->usable() || connection.value()->inTransaction()) return ErrorCode::InternalError;
        return std::unique_ptr<TransferMetadataStore>(new detail::MetadataStoreImpl(std::move(connection).value(), limits));
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}
Result<std::unique_ptr<MetadataStore>> openSqliteMetadataStore(const std::filesystem::path& path, SqliteMetadataOptions options) {
    auto store = openSqliteTransferMetadataStore(path, options);
    if (!store) return store.error();
    return std::unique_ptr<MetadataStore>(std::move(store).value());
}
}  // namespace anof::infrastructure::sqlite
