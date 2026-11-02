#include <anof/domain/drive/TreePolicy.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <anof/domain/drive/Blob.h>
#include <anof/domain/drive/NodeAccounting.h>
#include <limits>
#include <set>

namespace anof {
namespace {
Result<UtcTime> deadline(UtcTime time, std::int64_t seconds) {
    const auto delta = seconds * 1000000;
    if (time.time_since_epoch().count() > INT64_MAX - delta) return ErrorCode::IntegrityError;
    return time + std::chrono::microseconds{delta};
}
Result<NodeAccounting> changeNodes(NodeAccounting value, NodeCount used, NodeCount reserved) {
    if (!validateNodeAccounting(value)) return ErrorCode::IntegrityError;
    if (used.value() > kInstanceNodeLimit || reserved.value() > kInstanceNodeLimit - used.value())
        return ErrorCode::NodeLimitReached;
    if (value.used == used && value.reserved == reserved) return value;
    auto revision = value.revision.add(1);
    if (!revision) return revision.error();
    return NodeAccounting{used, reserved, revision.value()};
}
}
Result<UtcTime> trashExpiry(UtcTime time) { return deadline(time, kTrashRetentionSeconds); }
Result<UtcTime> blobGcDeadline(UtcTime time) { return deadline(time, kBlobGraceSeconds); }
Result<void> validateTrashEntry(const TrashEntry& entry) {
    auto expiry = trashExpiry(entry.deletedAt);
    if (!expiry || expiry.value() != entry.expiresAt || entry.nodeCount.value() < 1 ||
        entry.nodeCount.value() > static_cast<std::int64_t>(kTreeOperationLimit) ||
        !validateDirectoryName(entry.originalName) || entry.rootNodeId == entry.originalParentId ||
        (entry.kind != NodeKind::File && entry.kind != NodeKind::Directory) ||
        (entry.kind == NodeKind::File && entry.nodeCount.value() != 1)) return ErrorCode::IntegrityError;
    return {};
}
bool trashExpired(const TrashEntry& entry, UtcTime now) noexcept { return now >= entry.expiresAt; }
Result<void> validateUnavailableEvidence(const UnavailableEvidence& e, const Blob& b) {
    if (e.reason == UnavailableReason::Missing)
        return !e.file && !e.sha256 ? Result<void>{} : Result<void>{ErrorCode::IntegrityError};
    if (!e.file) return ErrorCode::IntegrityError;
    if (e.reason == UnavailableReason::Length)
        return e.file->size.value() != b.size.value() && !e.sha256 ? Result<void>{} : Result<void>{ErrorCode::IntegrityError};
    if (e.reason == UnavailableReason::Hash)
        return e.file->size.value() == b.size.value() && e.sha256 && *e.sha256 != b.sha256
            ? Result<void>{} : Result<void>{ErrorCode::IntegrityError};
    return ErrorCode::IntegrityError;
}
Result<ByteCount> unavailableCharge(const Blob& b, const UnavailableEvidence& e) {
    if (!validateUnavailableEvidence(e, b)) return ErrorCode::IntegrityError;
    return ByteCount::from(std::max(b.size.value(), e.file ? e.file->size.value() : 0));
}
Result<void> validateBlobRecord(const BlobRecord& b) {
    if (b.state == BlobState::Unavailable) {
        if (!b.unavailable || b.deleteStartedAt) return ErrorCode::IntegrityError;
        auto charge = unavailableCharge(b.content, *b.unavailable);
        if (!charge || b.chargedBytes != charge.value()) return ErrorCode::IntegrityError;
    } else if (b.unavailable || b.chargedBytes.value() != b.content.size.value()) return ErrorCode::IntegrityError;
    if (b.updatedAt < b.createdAt || (b.unreferencedAt && (*b.unreferencedAt < b.createdAt ||
        *b.unreferencedAt > b.updatedAt || !blobGcDeadline(*b.unreferencedAt)))) return ErrorCode::IntegrityError;
    if (b.state == BlobState::Available) {
        if (b.deleteStartedAt) return ErrorCode::IntegrityError;
    } else if (b.state == BlobState::Deleting) {
        if (!b.unreferencedAt || !b.deleteStartedAt || *b.deleteStartedAt < *b.unreferencedAt ||
            *b.deleteStartedAt > b.updatedAt || *b.deleteStartedAt < blobGcDeadline(*b.unreferencedAt).value()) return ErrorCode::IntegrityError;
    } else if (b.state != BlobState::Unavailable) return ErrorCode::IntegrityError;
    return {};
}
bool blobGcEligible(const BlobRecord& b, UtcTime now, bool nodeRef, bool uploadRef) noexcept {
    if (nodeRef || uploadRef || b.state != BlobState::Available || !b.unreferencedAt || !validateBlobRecord(b)) return false;
    auto due = blobGcDeadline(*b.unreferencedAt);
    return due && now >= due.value();
}
Result<void> validateStorageStatistics(const GcStatistics& s, ByteCount retained) {
    if (s.deletingBytes.value() > s.pendingBytes.value() || s.deletingObjects.value() > s.pendingObjects.value())
        return ErrorCode::IntegrityError;
    auto sum = s.pendingBytes.add(s.quarantinedBytes.value());
    if (!sum) return sum.error();
    sum = sum.value().add(s.unavailableBytes.value());
    if (!sum || sum.value().value() > retained.value()) return ErrorCode::IntegrityError;
    auto count = s.pendingObjects.add(s.quarantinedObjects.value());
    if (!count || !count.value().add(s.unavailableObjects.value())) return ErrorCode::IntegrityError;
    if ((!s.pendingObjects.value() && s.pendingBytes.value()) || (!s.deletingObjects.value() && s.deletingBytes.value()) ||
        (!s.quarantinedObjects.value() && s.quarantinedBytes.value()) || (!s.unavailableObjects.value() && s.unavailableBytes.value()))
        return ErrorCode::IntegrityError;
    return {};
}
Result<void> validateNodeAccounting(const NodeAccounting& n) {
    if (n.used.value() > kInstanceNodeLimit || n.reserved.value() > kInstanceNodeLimit - n.used.value())
        return ErrorCode::IntegrityError;
    return {};
}
Result<NodeAccounting> chargeNodes(NodeAccounting n, NodeCount count) {
    auto used = n.used.add(count.value());
    if (!used) return used.error();
    return changeNodes(n, used.value(), n.reserved);
}
Result<NodeAccounting> reserveNodes(NodeAccounting n, NodeCount count) {
    auto reserved = n.reserved.add(count.value());
    if (!reserved) return reserved.error();
    return changeNodes(n, n.used, reserved.value());
}
Result<NodeAccounting> publishNodes(NodeAccounting n, NodeCount count) {
    if (count.value() > n.reserved.value()) return ErrorCode::IntegrityError;
    auto used = n.used.add(count.value());
    if (!used) return used.error();
    return changeNodes(n, used.value(), NodeCount::from(n.reserved.value() - count.value()).value());
}
Result<NodeAccounting> releaseUsedNodes(NodeAccounting n, NodeCount count) {
    if (count.value() > n.used.value()) return ErrorCode::IntegrityError;
    return changeNodes(n, NodeCount::from(n.used.value() - count.value()).value(), n.reserved);
}
Result<NodeAccounting> releaseReservedNodes(NodeAccounting n, NodeCount count) {
    if (count.value() > n.reserved.value()) return ErrorCode::IntegrityError;
    return changeNodes(n, n.used, NodeCount::from(n.reserved.value() - count.value()).value());
}
Result<void> validateQueryBudget(const QueryBudget& b) {
    if (b.elapsed.count() <= 0 || b.elapsed > std::chrono::milliseconds{100} || b.instructions == 0 ||
        b.instructions > 10000000 || b.candidates == 0 || b.candidates > kSearchCandidateLimit)
        return ErrorCode::InvalidArgument;
    return {};
}
Result<SearchText> SearchText::parse(std::string_view text) {
    if (text.empty() || text.size() > 128) return ErrorCode::InvalidArgument;
    // 复用名称的UTF-8/Cc校验，但搜索允许点、路径符号及通配符的字面值。
    std::string check(text);
    for (auto& c : check) if (c == '/' || c == '\\') c = 'x';
    if (check == "." || check == "..") check.assign(check.size(), 'x');
    if (!validateDirectoryName(check)) return ErrorCode::InvalidArgument;
    return SearchText(std::string(text));
}
Result<TreeSummary> summarizeTree(const TreeSnapshot& tree) {
    if (tree.members.size() > kTreeOperationLimit) return ErrorCode::TreeLimitExceeded;
    if (tree.members.empty()) return ErrorCode::IntegrityError;
    std::map<NodeId, const TreeMember*> nodes;
    for (const auto& m : tree.members) if (!nodes.emplace(m.node.metadata.id, &m).second) return ErrorCode::IntegrityError;
    auto root = nodes.find(tree.root);
    if (root == nodes.end()) return ErrorCode::IntegrityError;
    const auto& base = root->second->node.metadata;
    TreeSummary result{{}, {}, base.createdBy, 0, base.updatedAt};
    for (const auto& m : tree.members) {
        const auto& n = m.node.metadata;
        if (n.spaceId != base.spaceId || n.updatedAt < n.createdAt || !validateDirectoryName(n.name)) return ErrorCode::IntegrityError;
        auto file = std::get_if<FileContentMetadata>(&m.node.content);
        if ((file && !FileLength::from(file->size.value())) || bool(file) != bool(m.blobId)) return ErrorCode::IntegrityError;
        if (result.soleCreator && *result.soleCreator != n.createdBy) result.soleCreator.reset();
        result.latestEvent = std::max(result.latestEvent, n.updatedAt);
        auto bytes = file ? file->size : ByteCount{};
        auto total = result.total.bytes.add(bytes.value());
        auto& contributor = result.byCreator[n.createdBy];
        auto owned = contributor.bytes.add(bytes.value());
        auto count = contributor.nodes.add(1);
        if (!total || !owned || !count) return ErrorCode::IntegrityError;
        result.total.bytes = total.value();
        contributor = {owned.value(), count.value()};
        std::set<NodeId> visited;
        const auto* current = &m;
        std::size_t depth = 0;
        while (current->node.metadata.id != tree.root) {
            if (!visited.insert(current->node.metadata.id).second || !current->node.metadata.parentId) return ErrorCode::IntegrityError;
            auto parent = nodes.find(*current->node.metadata.parentId);
            if (parent == nodes.end() || !std::holds_alternative<DirectoryContent>(parent->second->node.content)) return ErrorCode::IntegrityError;
            current = parent->second;
            if (++depth > kMaxDirectoryDepth) return ErrorCode::IntegrityError;
        }
        result.height = std::max(result.height, depth);
    }
    if (base.parentId && nodes.count(*base.parentId)) return ErrorCode::IntegrityError;
    result.total.nodes = NodeCount::from(static_cast<std::int64_t>(tree.members.size())).value();
    return result;
}
Result<void> requireTreeMutation(const Principal& principal, const Space& space, const TreeSnapshot& tree) {
    auto permission = requireCreatePermission(principal, space);
    if (!permission) return permission;
    if (tree.root == space.rootNodeId) return ErrorCode::Forbidden;
    auto summary = summarizeTree(tree);
    if (!summary) return summary.error();
    if (tree.members.front().node.metadata.spaceId != space.id) return ErrorCode::IntegrityError;
    if (space.kind == SpaceKind::Public && principal.role != UserRole::Administrator &&
        (!summary.value().soleCreator || *summary.value().soleCreator != principal.userId)) return ErrorCode::Forbidden;
    return {};
}
Result<void> validateTreeDestination(const TreeSnapshot& tree, const DirectoryPath& path) {
    auto summary = summarizeTree(tree);
    if (!summary) return summary.error();
    if (path.empty()) return ErrorCode::NotFound;
    const auto space = tree.members.front().node.metadata.spaceId;
    std::set<NodeId> seen;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto& n = path[i];
        if (n.spaceId != space) return ErrorCode::CrossSpaceMove;
        if (!seen.insert(n.id).second || (i == 0 ? bool(n.parentId) : (!n.parentId || *n.parentId != path[i-1].id)))
            return ErrorCode::IntegrityError;
        if (n.id == tree.root) return ErrorCode::DirectoryCycle;
    }
    if (path.size() > kMaxDirectoryDepth || summary.value().height > kMaxDirectoryDepth - path.size())
        return ErrorCode::DepthLimitExceeded;
    return {};
}
}  // namespace anof
