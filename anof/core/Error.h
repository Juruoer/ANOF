#pragma once

namespace anof {

enum class ErrorCode {
    InvalidArgument,
    InvalidName,
    LoginConflict,
    InviteUnavailable,
    UserLimitReached,
    Forbidden,
    PreconditionFailed,
    PreconditionRequired,
    SessionLimitReached,
    RateLimited,  // 已接纳的多阶段任务可在身份读取后触发限流。
    Unauthenticated,
    NotFound,
    ParentNotDirectory,
    DepthLimitExceeded,
    NameConflict,
    QuotaExceeded,
    UploadKeyConflict,
    UploadOffsetMismatch,
    UploadLengthMismatch,
    UploadBusy,
    UploadIncomplete,
    UploadStateConflict,
    UploadCommitInProgress,
    StorageBudgetExceeded,
    UploadTerminated,
    UploadLimitReached,
    NotAFile,
    Conflict,
    Retryable,
    StorageFull,
    CommitOutcomeUnknown,
    IntegrityError,
    InternalError,
    CrossSpaceMove,
    DirectoryCycle,
    TreeLimitExceeded,
    NodeLimitReached,
    RestoreTargetUnavailable,
    QueryLimitExceeded
};

// 端口只传播稳定分类；后端诊断由适配层记录，不能泄漏到业务响应。
struct Error {
    ErrorCode code;
};

}  // namespace anof
