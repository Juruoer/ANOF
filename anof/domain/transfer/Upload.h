#pragma once
#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <anof/domain/transfer/TransferValues.h>

#include <optional>

namespace anof {
enum class UploadState { Creating,
                         Open,
                         Verifying,
                         Ready,
                         Committing,
                         Completed,
                         Cancelled,
                         Expired,
                         Failed };
enum class StagingLocation { Temporary,
                             Object };
enum class TransferFailure {
    NameConflict,
    ParentNotDirectory,
    DepthLimitExceeded,
    NotFound,
    ChecksumMismatch,
    UploadCancelled,
    UploadExpired,
    UserDisabled,
    StorageFull,
    TemporarilyUnavailable,
    InternalError,
    CommitOutcomeUnknown
};
struct UploadTarget {
    NodeId parentId;
    std::string name;
    friend bool operator==(const UploadTarget& a, const UploadTarget& b) {
        return a.parentId == b.parentId && a.name == b.name;
    }
};
struct UploadCreateParameters {
    SpaceId spaceId;
    UploadTarget target;
    FileLength length;
    std::optional<Sha256> expectedSha256;
    friend bool operator==(const UploadCreateParameters& a, const UploadCreateParameters& b) {
        return a.spaceId == b.spaceId && a.target == b.target && a.length == b.length && a.expectedSha256 == b.expectedSha256;
    }
};
struct Upload {
    UploadId id;
    UserId userId;
    OperationId clientUploadId;
    UploadCreateParameters original;
    UploadTarget target;
    UploadOffset durableOffset;
    UploadState state;
    UploadGeneration generation;
    std::optional<Sha256> actualSha256;
    std::optional<BlobId> pendingBlobId;
    StagingLocation location;
    std::optional<NodeId> resultNodeId;
    UtcTime createdAt;
    UtcTime lastProgressAt;
    UtcTime expiresAt;
    std::optional<UtcTime> terminalAt;
    std::optional<TransferFailure> lastError;
    bool cancelRequested = false;
    bool cleanupPending = false;
    std::optional<UtcTime> resultDeletedAt = {};
};
struct UploadPosition {
    UtcTime createdAt;
    UploadId id;
};
struct UploadCounts {
    std::size_t activeUser, activeGlobal, retainedUser, retainedGlobal;
};
struct UploadMatch {
    UploadState state;
    UploadGeneration generation;
    UploadOffset offset;
};
inline constexpr std::size_t kActiveUploadsPerUser = 8, kActiveUploadsGlobal = 64;
inline constexpr std::size_t kRetainedUploadsPerUser = 512, kRetainedUploadsGlobal = 4096;
}  // namespace anof
