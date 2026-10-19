#pragma once
#include <anof/application/ports/TransferIdGenerator.h>

#include <memory>
namespace anof::infrastructure::crypto {
Result<std::unique_ptr<TransferIdGenerator>> makeSodiumTransferIdGenerator();
}
