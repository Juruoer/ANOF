#pragma once
#include <anof/application/maintenance/Initialization.h>
#include <anof/domain/transfer/TransferValues.h>

namespace anof {
// 当前初始化入口要求显式预算；磁盘余量统一通过 InstanceConfiguration 传递。
struct TransferInitOptions {
    TransferInitOptions(InitOptions options, ByteCount budget)
        : instance(std::move(options)), contentBudget(budget) {}
    InitOptions instance;
    ByteCount contentBudget;
};
struct TransferBootstrapSeed {
    TransferBootstrapSeed(BootstrapSeed seed, ByteCount budget)
        : identity(std::move(seed)), contentBudget(budget) {}
    BootstrapSeed identity;
    ByteCount contentBudget;
};
}  // namespace anof
