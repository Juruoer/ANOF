#pragma once
#include <anof/domain/transfer/Upload.h>

namespace anof {
enum class TransferEventKind { Create,
                               AcceptComplete,
                               Publish,
                               Cancel,
                               Expire,
                               Fail,
                               StorageLimit };
struct TransferEvent {
    TransferEventKind kind;
    RequestId requestId;
    // 后台恢复事件可无交互 actor，但必须有 upload；预算变更必须有 actor。
    std::optional<UserId> actor;
    std::optional<UploadId> upload;
    UtcTime occurredAt;
    std::optional<TransferFailure> failure;
};
class TransferAuditRepository {
public:
    virtual ~TransferAuditRepository() = default;
    virtual Result<void> append(const TransferEvent&) = 0;
};
}  // namespace anof
