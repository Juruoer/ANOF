#pragma once
#include <anof/application/ports/MetadataInspection.h>
namespace anof::infrastructure::sqlite {
Result<std::unique_ptr<MetadataInspection>> makeSqliteMetadataInspection();
}
