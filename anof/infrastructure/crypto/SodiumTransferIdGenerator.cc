#include <anof/infrastructure/crypto/SodiumTransferIdGenerator.h>
#include <anof/infrastructure/crypto/internal/RandomUuid.h>
#include <anof/infrastructure/crypto/internal/SodiumSupport.h>
#include <sodium.h>

#include <new>
namespace anof::infrastructure::crypto {
namespace {
class Generator final : public TransferIdGenerator, public DriveIdGenerator {
    template <class T>
    Result<T> next() {
        std::array<unsigned char, 16> bytes{};
        randombytes_buf(bytes.data(), bytes.size());
        return detail::uuidFromRandomBytes<T>(bytes);
    }

public:
    Result<TrashId> trashId() override { return next<TrashId>(); }
    Result<UploadId> uploadId() override { return next<UploadId>(); }
    Result<BlobId> blobId() override { return next<BlobId>(); }
};
}  // namespace
Result<std::unique_ptr<TransferIdGenerator>> makeSodiumTransferIdGenerator() {
    auto initialized = detail::initializeSodium();
    if (!initialized) return initialized.error();
    try {
        return std::unique_ptr<TransferIdGenerator>(new Generator);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}
Result<std::unique_ptr<DriveIdGenerator>> makeSodiumDriveIdGenerator() {
    auto ready = detail::initializeSodium();
    if (!ready) return ready.error();
    try {
        return std::unique_ptr<DriveIdGenerator>(new Generator);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}
}  // namespace anof::infrastructure::crypto
