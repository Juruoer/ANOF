#include <anof/application/maintenance/Initialization.h>

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
}  // namespace anof
