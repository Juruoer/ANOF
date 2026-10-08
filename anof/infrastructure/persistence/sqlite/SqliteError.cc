#include <anof/infrastructure/persistence/sqlite/SqliteError.h>

#include <sqlite3.h>

namespace anof::infrastructure::sqlite {

Error sqliteError(int extendedCode, SqliteOperation operation, bool knownNotCommitted) {
    if (operation == SqliteOperation::Commit && !knownNotCommitted) return { ErrorCode::CommitOutcomeUnknown };
    switch (extendedCode & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return { ErrorCode::Retryable };
    case SQLITE_FULL:
        return { ErrorCode::StorageFull };
    case SQLITE_CONSTRAINT:
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_MISMATCH:
        return { ErrorCode::IntegrityError };
    default:
        return { ErrorCode::InternalError };
    }
}

}  // namespace anof::infrastructure::sqlite
