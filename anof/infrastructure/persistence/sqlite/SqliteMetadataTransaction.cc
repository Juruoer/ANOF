#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>

namespace anof::infrastructure::sqlite {

SqliteMetadataTransaction::SqliteMetadataTransaction(SqliteConnection& connection, bool& storeActive, PasswordHashLimits limits)
    : context_(connection, storeActive), authorization_(context_), spaces_(context_), nodes_(context_),
      quotas_(context_), audit_(context_), users_(context_, limits), sessions_(context_),
      invites_(context_), settings_(context_), identityAudit_(context_) {}

SqliteMetadataTransaction::~SqliteMetadataTransaction() = default;
Result<void> SqliteMetadataTransaction::begin() { return context_.begin(); }

}  // namespace anof::infrastructure::sqlite
