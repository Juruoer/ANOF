#include <anof/infrastructure/crypto/SodiumMaintenanceDigest.h>
#include <sodium.h>
namespace anof::infrastructure::crypto {
Result<Sha256> SodiumMaintenanceDigest::digest(std::string_view bytes) {
    std::array<std::uint8_t, 32> hash{};
    if (sodium_init() < 0 || crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size())) return ErrorCode::InternalError;
    return Sha256(hash);
}
}  // namespace anof::infrastructure::crypto
