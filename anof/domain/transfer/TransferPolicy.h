#pragma once
#include <anof/domain/transfer/Upload.h>

namespace anof {
Result<void> requireUploadCapacity(const UploadCounts&);
bool isTerminal(UploadState) noexcept;
bool mayTransition(UploadState from, UploadState to) noexcept;
Result<UtcTime> uploadExpiry(UtcTime created, UtcTime lastProgress);
Result<void> validateUpload(const Upload&);
// 持久化不变量检查，不替代事务内授权、独占资格和业务状态机。
Result<void> validateUploadReplacement(const Upload& before, const Upload& after);
bool mayForgetUpload(const Upload&, UtcTime now) noexcept;
}  // namespace anof
