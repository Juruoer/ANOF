#pragma once
#include <anof/application/ports/MetadataRepair.h>
namespace anof::infrastructure::sqlite {
Result<std::unique_ptr<MetadataRepair>> makeSqliteMetadataRepair();
}
