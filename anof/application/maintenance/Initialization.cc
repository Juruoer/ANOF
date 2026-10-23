#include <anof/application/maintenance/Initialization.h>
#include <array>

namespace anof {
Result<void> validateInitialQuotas(const InitialQuotas& quotas) {
    // 私有根与公共根各占一个节点；公共根计入管理员贡献，三组限额均必须容纳它。
    if (quotas.privateSpace.nodes.value() < 1 || quotas.publicSpace.nodes.value() < 1 ||
        quotas.contribution.nodes.value() < 1) return ErrorCode::InvalidArgument;
    return {};
}
Result<void> validateBootstrapSeed(const BootstrapSeed& seed) {
    if (seed.publicSpaceId == seed.privateSpaceId || seed.publicRootId == seed.privateRootId ||
        seed.encodedPassword.view().empty()) return ErrorCode::InvalidArgument;
    return validateInitialQuotas(seed.quotas);
}
Result<NodeAccounting> initialNodeAccounting(const BootstrapSeed& seed) {
    auto valid = validateBootstrapSeed(seed);
    if (!valid) return valid.error();
    const std::array<NodeId, 2> roots{seed.publicRootId, seed.privateRootId};
    return NodeAccounting{NodeCount::from(static_cast<std::int64_t>(roots.size())).value(), {}, Revision{}};
}
}  // namespace anof
