#include <anof/domain/transfer/TransferPolicy.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <algorithm>

namespace anof {
Result<void> requireUploadCapacity(const UploadCounts& counts) {
    if (counts.activeUser > counts.activeGlobal || counts.retainedUser > counts.retainedGlobal ||
        counts.activeUser > counts.retainedUser || counts.activeGlobal > counts.retainedGlobal)
        return ErrorCode::IntegrityError;
    if (counts.activeUser >= kActiveUploadsPerUser || counts.activeGlobal >= kActiveUploadsGlobal ||
        counts.retainedUser >= kRetainedUploadsPerUser || counts.retainedGlobal >= kRetainedUploadsGlobal)
        return ErrorCode::UploadLimitReached;
    return {};
}
bool isTerminal(UploadState s) noexcept {
    return s == UploadState::Completed || s == UploadState::Cancelled || s == UploadState::Expired || s == UploadState::Failed;
}
bool mayTransition(UploadState from, UploadState to) noexcept {
    if (from == to) return true;
    if (isTerminal(from)) return false;
    if (to == UploadState::Cancelled || to == UploadState::Failed) return true;
    if (to == UploadState::Expired) return from != UploadState::Committing;
    switch (from) {
        case UploadState::Creating: return to == UploadState::Open;
        case UploadState::Open: return to == UploadState::Verifying;
        case UploadState::Verifying: return to == UploadState::Ready;
        case UploadState::Ready: return to == UploadState::Committing;
        case UploadState::Committing: return to == UploadState::Ready || to == UploadState::Completed;
        default: return false;
    }
}
Result<UtcTime> uploadExpiry(UtcTime created, UtcTime progress) {
    constexpr std::int64_t day = 86400LL * 1000000;
    const auto c = created.time_since_epoch().count(), p = progress.time_since_epoch().count();
    const auto max = std::numeric_limits<std::int64_t>::max();
    if (p < c || c > max - 30 * day || p > max - 7 * day) return ErrorCode::IntegrityError;
    return UtcTime(std::chrono::microseconds(std::min(c + 30 * day, p + 7 * day)));
}
Result<void> validateUpload(const Upload& u) {
    if (u.resultDeletedAt && (u.state != UploadState::Completed || !u.terminalAt ||
        *u.resultDeletedAt < *u.terminalAt)) return ErrorCode::IntegrityError;
    switch (u.state) {
        case UploadState::Creating: case UploadState::Open: case UploadState::Verifying:
        case UploadState::Ready: case UploadState::Committing: case UploadState::Completed:
        case UploadState::Cancelled: case UploadState::Expired: case UploadState::Failed: break;
        default: return ErrorCode::IntegrityError;
    }
    if (u.location != StagingLocation::Temporary && u.location != StagingLocation::Object)
        return ErrorCode::IntegrityError;
    auto expiry = uploadExpiry(u.createdAt, u.lastProgressAt);
    if (!expiry || expiry.value() != u.expiresAt || !validateDirectoryName(u.original.target.name) ||
        !validateDirectoryName(u.target.name) || u.durableOffset.value() > u.original.length.value() ||
        isTerminal(u.state) != u.terminalAt.has_value() ||
        (u.terminalAt && *u.terminalAt < u.lastProgressAt)) return ErrorCode::IntegrityError;
    const bool published = u.state == UploadState::Completed;
    const bool verified = u.state == UploadState::Ready || u.state == UploadState::Committing || published;
    if (published != u.resultNodeId.has_value() || (verified && !u.actualSha256) ||
        ((u.state == UploadState::Verifying || verified) && u.durableOffset.value() != u.original.length.value()) ||
        (u.state == UploadState::Creating && u.durableOffset.value() != 0) ||
        ((u.location == StagingLocation::Object || u.state == UploadState::Committing || published) && !u.pendingBlobId) ||
        (u.cleanupPending && (!isTerminal(u.state) || published)) ||
        (verified && u.original.expectedSha256 && u.actualSha256 != u.original.expectedSha256)) return ErrorCode::IntegrityError;
    if (((u.state == UploadState::Creating || u.state == UploadState::Open || u.state == UploadState::Verifying) &&
         (u.actualSha256 || u.pendingBlobId || u.location != StagingLocation::Temporary)) ||
        (published && (u.location != StagingLocation::Object || u.cancelRequested || u.lastError)) ||
        (u.state == UploadState::Creating && !(u.target == u.original.target))) return ErrorCode::IntegrityError;
    return {};
}
Result<void> validateUploadReplacement(const Upload& before, const Upload& after) {
    if (!validateUpload(before) || !validateUpload(after)) return ErrorCode::IntegrityError;
    if ((before.resultDeletedAt && before.resultDeletedAt != after.resultDeletedAt) ||
        (!before.resultDeletedAt && after.resultDeletedAt && before.state != UploadState::Completed))
        return ErrorCode::IntegrityError;
    auto next = before.generation.add(1);
    if (!next || next.value() != after.generation || before.id != after.id || before.userId != after.userId ||
        before.clientUploadId != after.clientUploadId || !(before.original == after.original) ||
        before.createdAt != after.createdAt || after.lastProgressAt < before.lastProgressAt ||
        after.durableOffset.value() < before.durableOffset.value() ||
        (before.cancelRequested && !after.cancelRequested) || !mayTransition(before.state, after.state)) return ErrorCode::IntegrityError;
    if ((before.pendingBlobId && before.pendingBlobId != after.pendingBlobId) ||
        (before.actualSha256 && before.actualSha256 != after.actualSha256) ||
        (before.location == StagingLocation::Object && after.location != before.location)) return ErrorCode::IntegrityError;
    if (!isTerminal(before.state) && isTerminal(after.state) && after.state != UploadState::Completed && !after.cleanupPending)
        return ErrorCode::IntegrityError;
    if ((after.durableOffset != before.durableOffset &&
         (before.state != UploadState::Open || after.state != UploadState::Open)) ||
        (!(before.target == after.target) &&
         !((before.state == UploadState::Open && after.state == UploadState::Verifying) ||
           (before.state == UploadState::Ready && (after.state == UploadState::Ready || after.state == UploadState::Committing)))))
        return ErrorCode::IntegrityError;
    if (isTerminal(before.state) && (!(before.target == after.target) || before.resultNodeId != after.resultNodeId ||
        before.terminalAt != after.terminalAt || before.durableOffset != after.durableOffset ||
        before.lastProgressAt != after.lastProgressAt || before.lastError != after.lastError ||
        (!before.cleanupPending && after.cleanupPending))) return ErrorCode::IntegrityError;
    return {};
}
bool mayForgetUpload(const Upload& u, UtcTime now) noexcept {
    constexpr std::int64_t retention = 7LL * 86400 * 1000000;
    if (!isTerminal(u.state) || u.cleanupPending || !u.terminalAt) return false;
    auto t = u.terminalAt->time_since_epoch().count();
    return t <= std::numeric_limits<std::int64_t>::max() - retention &&
        now.time_since_epoch().count() >= t + retention;
}
}  // namespace anof
