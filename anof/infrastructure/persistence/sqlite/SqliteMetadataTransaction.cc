#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>

namespace anof::infrastructure::sqlite {

SqliteMetadataTransaction::SqliteMetadataTransaction(SqliteConnection& connection, bool& storeActive, PasswordHashLimits limits)
    : context_(connection, storeActive), tree_(context_), trash_(context_), nodeAccounting_(context_), driveAudit_(context_), authorization_(context_), spaces_(context_), nodes_(context_), quotas_(context_), audit_(context_), users_(context_, limits), sessions_(context_), invites_(context_), settings_(context_), identityAudit_(context_), uploads_(context_), blobs_(context_), storage_(context_), transferAudit_(context_) {}

SqliteMetadataTransaction::~SqliteMetadataTransaction() = default;
Result<void> SqliteMetadataTransaction::begin() { return context_.begin(); }

}  // namespace anof::infrastructure::sqlite
