#include <anof/application/drive/PurgeTree.h>
#include <algorithm>
#include <set>
namespace anof {
Result<void> purgeTree(DriveTransaction& tx, const TrashEntry& entry, UtcTime now, RequestId request, DriveActor actor) {
    auto tree = tx.tree().trashSubtree(entry.id, {}); if (!tree) return tree.error();
    auto summary = summarizeTree(tree.value()); if (!summary) return summary.error();
    auto space = tx.spaces().find(entry.spaceId); if (!space) return space.error();
    if (!space.value()) return ErrorCode::IntegrityError;
    Result<void> valid;
    const auto at = std::max({now, entry.deletedAt, summary.value().latestEvent});
    std::vector<NodeId> results; std::set<BlobId> blobs;
    for (const auto& m : tree.value().members) { results.push_back(m.node.metadata.id); if (m.blobId) blobs.insert(*m.blobId); }
    auto marked = tx.uploads().markResultsDeleted(results, at); if (!marked) return marked.error();
    valid = tx.tree().eraseMembers(TrashCondition{entry.id, entry.revision}, tree.value()); if (!valid) return valid.error();
    valid = tx.trash().erase(TrashCondition{entry.id, entry.revision}); if (!valid) return valid.error();
    valid = tx.quotas().releaseUsed(SpaceQuota{space.value()->id}, summary.value().total); if (!valid) return valid.error();
    if (space.value()->kind == SpaceKind::Public) for (const auto& pair : summary.value().byCreator) {
        valid = tx.quotas().releaseUsed(PublicContributionQuota{space.value()->id, pair.first}, pair.second); if (!valid) return valid.error();
    }
    valid = tx.nodeAccounting().releaseUsed(summary.value().total.nodes); if (!valid) return valid.error();
    for (const auto id : blobs) {
        auto held = tx.blobs().hasNodeReference(id); if (!held) return held.error();
        if (held.value()) continue;
        auto record = tx.blobs().record(id); if (!record) return record.error();
        if (!record.value()) return ErrorCode::IntegrityError;
        auto changed = tx.blobs().markUnreferenced(id, record.value()->revision, at); if (!changed) return changed.error();
    }
    return tx.driveAudit().append({request, now, actor, TreePurged{entry.id, entry.rootNodeId}});
}
}
