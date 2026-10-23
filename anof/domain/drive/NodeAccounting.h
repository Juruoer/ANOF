#pragma once
#include <anof/core/Quantity.h>
namespace anof {
inline constexpr std::int64_t kInstanceNodeLimit = 100000;
struct GcStatistics { ByteCount pendingBytes; NodeCount pendingObjects; ByteCount deletingBytes; NodeCount deletingObjects; };
struct NodeAccounting { NodeCount used; NodeCount reserved; Revision revision; };
Result<void> validateNodeAccounting(const NodeAccounting&);
Result<NodeAccounting> chargeNodes(NodeAccounting, NodeCount);
Result<NodeAccounting> reserveNodes(NodeAccounting, NodeCount);
Result<NodeAccounting> publishNodes(NodeAccounting, NodeCount);
Result<NodeAccounting> releaseUsedNodes(NodeAccounting, NodeCount);
Result<NodeAccounting> releaseReservedNodes(NodeAccounting, NodeCount);
}  // namespace anof
