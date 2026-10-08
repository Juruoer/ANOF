#pragma once

namespace anof {

enum class ErrorCode {
    InvalidArgument,
    InvalidName,
    Unauthenticated,
    NotFound,
    ParentNotDirectory,
    DepthLimitExceeded,
    NameConflict,
    QuotaExceeded,
    Conflict,
    Retryable,
    StorageFull,
    CommitOutcomeUnknown,
    IntegrityError,
    InternalError
};

// 端口只传播稳定分类；后端诊断由适配层记录，不能泄漏到业务响应。
struct Error {
    ErrorCode code;
};

}  // namespace anof
