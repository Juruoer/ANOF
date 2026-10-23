#pragma once
#include <anof/core/Clock.h>
#include <anof/core/Id.h>
#include <optional>
#include <variant>
namespace anof {
struct NodeRenamed { NodeId id; };
struct NodeMoved { NodeId id; NodeId from; NodeId to; };
struct FileCopied { NodeId source; NodeId result; };
struct TreeTrashed { TrashId id; NodeId root; };
struct TreeRestored { TrashId id; NodeId root; };
struct TreePurged { TrashId id; NodeId root; };
struct GcMarked { BlobId id; };
struct GcFinished { BlobId id; };
struct MaintenanceActor {};
using DriveActor = std::variant<UserId, MaintenanceActor>;
using DriveAction = std::variant<NodeRenamed, NodeMoved, FileCopied, TreeTrashed, TreeRestored, TreePurged, GcMarked, GcFinished>;
struct DriveEvent { RequestId requestId; UtcTime observedAt; DriveActor actor; DriveAction action; };
class DriveAuditRepository {
public:
    virtual ~DriveAuditRepository() = default;
    virtual Result<void> append(const DriveEvent&) = 0;
};
}  // namespace anof
