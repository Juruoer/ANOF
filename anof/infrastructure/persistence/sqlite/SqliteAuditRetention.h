#pragma once
#include <anof/application/ports/AuditRetention.h>
#include <anof/infrastructure/persistence/sqlite/SqliteOptions.h>
namespace anof::infrastructure::sqlite {
Result<std::unique_ptr<AuditRetention>> openSqliteAuditRetention(const std::filesystem::path&, SqliteOptions = {});
}
