#include <anof/infrastructure/crypto/SodiumIdentityIdGenerator.h>
#include <anof/infrastructure/crypto/internal/RandomUuid.h>
#include <anof/infrastructure/crypto/internal/SodiumSupport.h>
#include <sodium.h>
#include <new>

namespace anof::infrastructure::crypto {
namespace {
class SodiumIdentityIdGenerator final : public IdentityIdGenerator {
public:
    Result<UserId> nextUser() override { return next<UserId>(); }
    Result<SpaceId> nextSpace() override { return next<SpaceId>(); }
    Result<NodeId> nextNode() override { return next<NodeId>(); }
    Result<InviteId> nextInvite() override { return next<InviteId>(); }
    Result<InstanceId> nextInstance() override { return next<InstanceId>(); }
    Result<OperationId> nextOperation() override { return next<OperationId>(); }
private:
    template <typename IdType>
    Result<IdType> next() {
        std::array<unsigned char, 16> bytes{};
        randombytes_buf(bytes.data(), bytes.size());
        return detail::uuidFromRandomBytes<IdType>(bytes);
    }
};
}  // namespace
Result<std::unique_ptr<IdentityIdGenerator>> makeSodiumIdentityIdGenerator() {
    auto initialized = detail::initializeSodium();
    if (!initialized) return initialized.error();
    try { return std::unique_ptr<IdentityIdGenerator>(new SodiumIdentityIdGenerator); }
    catch (const std::bad_alloc&) { return ErrorCode::InternalError; }
}
}  // namespace anof::infrastructure::crypto
