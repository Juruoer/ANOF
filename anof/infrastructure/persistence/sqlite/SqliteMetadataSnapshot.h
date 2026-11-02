#pragma once
#include <anof/application/ports/MetadataSnapshot.h>
#include <anof/domain/identity/Secrets.h>
namespace anof::infrastructure::sqlite {
Result<std::unique_ptr<MetadataSnapshot>> makeSqliteMetadataSnapshot(PasswordHashLimits);
}
