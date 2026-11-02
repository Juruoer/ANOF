#pragma once
#include <anof/core/Quantity.h>
namespace anof {
inline constexpr std::int64_t kInstanceNodeLimit = 100000;
struct GcStatistics {
    ByteCount pendingBytes;
    NodeCount pendingObjects;
    ByteCount deletingBytes;
    NodeCount deletingObjects;
    ByteCount quarantinedBytes;
    NodeCount quarantinedObjects;
    ByteCount unavailableBytes;
    NodeCount unavailableObjects;
};
// pending/quarantine/unavailable互不重叠；deleting为pending子集。
Result<void> validateStorageStatistics(const GcStatistics&, ByteCount retained);
struct NodeAccounting {
    NodeCount used;
    NodeCount reserved;
    Revision revision;
};
Result<void> validateNodeAccounting(const NodeAccounting&);
Result<NodeAccounting> chargeNodes(NodeAccounting, NodeCount);
Result<NodeAccounting> reserveNodes(NodeAccounting, NodeCount);
Result<NodeAccounting> publishNodes(NodeAccounting, NodeCount);
Result<NodeAccounting> releaseUsedNodes(NodeAccounting, NodeCount);
Result<NodeAccounting> releaseReservedNodes(NodeAccounting, NodeCount);
}  // namespace anof
