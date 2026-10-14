#include <anof/infrastructure/crypto/internal/NodeUuid.h>
#include <anof/infrastructure/crypto/internal/RandomUuid.h>

namespace anof::infrastructure::crypto::detail {
Result<NodeId> nodeIdFromRandomBytes(std::array<unsigned char, 16> bytes) {
    return uuidFromRandomBytes<NodeId>(bytes);
}
}  // namespace anof::infrastructure::crypto::detail
