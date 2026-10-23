#include <anof/application/transfer/TransferRecovery.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <anof/application/ports/DriveMetadataStore.h>
#include <functional>

namespace anof {
namespace {
// 仅供持锁启动校验：用完整时间域枚举所有条目，不执行到期清理，也不伪造用户Session。
// 每页16项、每棵树最多1000成员，不把全实例节点载入内存。
Result<void> visitTrash(DriveReadSnapshot& r, const std::function<Result<void>(const TrashEntry&, const TreeSnapshot&)>& visit) {
    std::optional<TrashExpiryPosition> after;
    do {
        auto page = r.trash().expired(UtcTime::max(), PageSize::from(16).value(), after);
        if (!page) return page.error();
        for (const auto& entry : page.value().items) {
            auto tree = r.tree().trashSubtree(entry.id, {}); if (!tree) return tree.error();
            auto summary = summarizeTree(tree.value()); if (!summary) return summary.error();
            if (summary.value().total.nodes != entry.nodeCount || summary.value().total.bytes != entry.logicalBytes ||
                summary.value().soleCreator != entry.soleCreatorId) return ErrorCode::IntegrityError;
            for (const auto& m : tree.value().members) {
                if (m.node.metadata.spaceId != entry.spaceId || m.node.metadata.updatedAt < entry.deletedAt) return ErrorCode::IntegrityError;
                if (auto* file = std::get_if<FileContentMetadata>(&m.node.content)) {
                    if (!m.blobId) return ErrorCode::IntegrityError;
                    auto blob = r.blobLifecycle().record(*m.blobId); if (!blob) return blob.error();
                    if (!blob.value() || blob.value()->state != BlobState::Available ||
                        blob.value()->content.size.value() != file->size.value() || blob.value()->content.sha256.bytes() != file->sha256)
                        return ErrorCode::IntegrityError;
                }
            }
            auto checked = visit(entry, tree.value()); if (!checked) return checked;
        }
        after = page.value().next;
    } while (after);
    return {};
}
struct Totals {
    ByteCount bytes;
    NodeCount nodes;
};
Result<void> add(Totals& value, std::int64_t bytes, std::int64_t nodes) {
    auto b = value.bytes.add(bytes);
    auto n = value.nodes.add(nodes);
    if (!b || !n) return ErrorCode::IntegrityError;
    value = { b.value(), n.value() };
    return {};
}
Result<void> countTree(TransferReadSnapshot& r, const Space& space, NodeId parent, std::optional<UserId> contributor,
                       Totals& total, unsigned depth = 0) {
    if (depth > 128) return ErrorCode::IntegrityError;
    auto node = r.nodes().find(parent);
    if (!node) return node.error();
    if (!node.value()) return ErrorCode::IntegrityError;
    if (node.value()->metadata.spaceId != space.id) return ErrorCode::IntegrityError;
    if (!contributor || node.value()->metadata.createdBy == *contributor) {
        auto a = add(total, 0, 1);
        if (!a) return a;
    }
    std::optional<ChildPosition> after;
    do {
        auto page = r.nodes().listChildren(space.id, parent, PageSize::from(64).value(), after);
        if (!page) return page.error();
        for (const auto& child : page.value().items) {
            if (auto* file = std::get_if<FileContentMetadata>(&child.content)) {
                auto blob = r.blobs().findByNode(child.metadata.id);
                if (!blob) return blob.error();
                if (!blob.value() || blob.value()->size.value() != file->size.value() ||
                    blob.value()->sha256.bytes() != file->sha256)
                    return ErrorCode::IntegrityError;
                if (!contributor || child.metadata.createdBy == *contributor) {
                    auto a = add(total, file->size.value(), 1);
                    if (!a) return a;
                }
            } else {
                auto a = countTree(r, space, child.metadata.id, contributor, total, depth + 1);
                if (!a) return a;
            }
        }
        after = page.value().next;
    } while (after);
    return {};
}
Result<Totals> reserved(TransferReadSnapshot& r, SpaceId space, std::optional<UserId> user, UtcTime now) {
    Totals total;
    std::optional<UploadPosition> after;
    do {
        auto page = r.uploads().scan(UploadScan::Active, now, PageSize::from(64).value(), after);
        if (!page) return page.error();
        for (const auto& u : page.value().items)
            if (u.original.spaceId == space && (!user || u.userId == *user)) {
                auto a = add(total, u.original.length.value(), 1);
                if (!a) return a.error();
            }
        after = page.value().next;
    } while (after);
    return total;
}
Result<void> checkQuota(DriveReadSnapshot& r, const Space& space, std::optional<UserId> user, UtcTime now) {
    auto res = reserved(r, space.id, user, now);
    if (!res) return res.error();
    Totals used;
    auto counted = countTree(r, space, space.rootNodeId, user, used);
    if (!counted) return counted;
    auto trashed = visitTrash(r, [&](const auto& entry, const auto& tree) -> Result<void> {
        if (entry.spaceId != space.id) return {};
        for (const auto& member : tree.members) {
            if (user && member.node.metadata.createdBy != *user) continue;
            auto* file = std::get_if<FileContentMetadata>(&member.node.content);
            auto a = add(used, file ? file->size.value() : 0, 1); if (!a) return a;
        }
        return {};
    });
    if (!trashed) return trashed;
    auto quota = r.quotas().get(user ? QuotaScope{ PublicContributionQuota{ space.id, *user } }
                                     : QuotaScope{ SpaceQuota{ space.id } });
    if (!quota) return quota.error();
    const auto& q = quota.value().state;
    if (q.reserved.bytes != res.value().bytes || q.reserved.nodes != res.value().nodes || q.used.bytes != used.bytes ||
        q.used.nodes != used.nodes)
        return ErrorCode::IntegrityError;
    return {};
}
}  // namespace
Result<void> auditTransferMetadata(TransferMetadataStore& store, UtcTime now) {
    auto* drive = dynamic_cast<DriveMetadataStore*>(&store);
    if (!drive) return ErrorCode::InternalError;
    auto opened = drive->beginDriveRead();
    if (!opened) return opened.error();
    auto& r = *opened.value();
    auto storage = r.storage().get();
    if (!storage) return storage.error();
    ByteCount retained, reservedBytes;
    std::optional<BlobId> blobAfter;
    do {
        auto page = r.blobs().list(PageSize::from(64).value(), blobAfter);
        if (!page) return page.error();
        for (const auto& b : page.value().items) {
            auto ref = r.blobs().hasNodeReference(b.id);
            if (!ref) return ref.error();
            auto record = r.blobLifecycle().record(b.id); if (!record) return record.error();
            if (!record.value()) return ErrorCode::IntegrityError;
            const auto& row = *record.value();
            auto upload = r.blobs().hasUploadReference(b.id); if (!upload) return upload.error();
            // 发布持有不应与已提交Blob双重占用同一个对象key。
            if (upload.value() || (ref.value() && (row.state != BlobState::Available || row.unreferencedAt)) ||
                (!ref.value() && !row.unreferencedAt)) return ErrorCode::IntegrityError;
            auto n = retained.add(b.size.value());
            if (!n) return ErrorCode::IntegrityError;
            retained = n.value();
        }
        blobAfter = page.value().next;
    } while (blobAfter);
    for (auto scan : { UploadScan::Active, UploadScan::CleanupPending }) {
        std::optional<UploadPosition> after;
        do {
            auto page = r.uploads().scan(scan, now, PageSize::from(64).value(), after);
            if (!page) return page.error();
            for (const auto& u : page.value().items) {
                auto n = reservedBytes.add(u.original.length.value());
                if (!n) return ErrorCode::IntegrityError;
                reservedBytes = n.value();
            }
            after = page.value().next;
        } while (after);
    }
    if (retained != storage.value().retained || reservedBytes != storage.value().reserved)
        return ErrorCode::IntegrityError;
    std::optional<UserListPosition> userAfter;
    bool publicChecked = false;
    NodeCount usedNodes, reservedNodes;
    do {
        auto users = r.users().list(PageSize::from(64).value(), userAfter);
        if (!users) return users.error();
        for (const auto& user : users.value().items) {
            auto spaces = r.spaces().listAccessible(user.id);
            if (!spaces) return spaces.error();
            for (const auto& space : spaces.value()) {
                if (space.kind == SpaceKind::Private || !publicChecked) {
                    auto q = checkQuota(r, space, {}, now);
                    if (!q) return q;
                    auto quota = r.quotas().get(SpaceQuota{space.id}); if (!quota) return quota.error();
                    auto used = usedNodes.add(quota.value().state.used.nodes.value());
                    auto reserved = reservedNodes.add(quota.value().state.reserved.nodes.value());
                    if (!used || !reserved) return ErrorCode::IntegrityError;
                    usedNodes = used.value(); reservedNodes = reserved.value();
                }
                if (space.kind == SpaceKind::Public) {
                    publicChecked = true;
                    auto q = checkQuota(r, space, user.id, now);
                    if (!q) return q;
                }
            }
            std::optional<UploadPosition> after;
            do {
                auto page = r.uploads().list(user.id, PageSize::from(64).value(), after);
                if (!page) return page.error();
                for (const auto& u : page.value().items)
                    if (u.state == UploadState::Completed) {
                        if (!u.resultNodeId || !u.pendingBlobId || !u.actualSha256) return ErrorCode::IntegrityError;
                        auto node = r.nodes().find(*u.resultNodeId); if (!node) return node.error();
                        auto blob = r.blobs().findByNode(*u.resultNodeId); if (!blob) return blob.error();
                        auto hidden = visitTrash(r, [&](const auto&, const auto& tree) -> Result<void> {
                            for (const auto& member : tree.members) if (member.node.metadata.id == *u.resultNodeId) {
                                if (node.value() || !member.blobId) return ErrorCode::IntegrityError;
                                node = std::optional<NodeDetails>{member.node};
                                blob = r.blobs().find(*member.blobId);
                                if (!blob) return blob.error();
                            }
                            return {};
                        });
                        if (!hidden) return hidden;
                        if (u.resultDeletedAt) {
                            if (node.value()) return ErrorCode::IntegrityError;
                        } else if (!node.value() || !blob.value() || blob.value()->id != *u.pendingBlobId ||
                            blob.value()->size != u.original.length || blob.value()->sha256 != *u.actualSha256 ||
                            node.value()->metadata.spaceId != u.original.spaceId || node.value()->metadata.createdBy != u.userId)
                            return ErrorCode::IntegrityError;
                    }
                after = page.value().next;
            } while (after);
        }
        userAfter = users.value().next;
    } while (userAfter);
    auto ledger = r.nodeAccounting().get(); if (!ledger) return ledger.error();
    if (ledger.value().used != usedNodes || ledger.value().reserved != reservedNodes) return ErrorCode::IntegrityError;
    return {};
}
Result<void> forgetTerminalUploads(TransferMetadataStore& store, UtcTime now) {
    auto opened = store.beginTransferWrite();
    if (!opened) return opened.error();
    auto& tx = *opened.value();
    auto page = tx.uploads().scan(UploadScan::RetentionElapsed, now, PageSize::from(100).value(), {});
    if (!page) return page.error();
    for (const auto& u : page.value().items) {
        auto erased = tx.uploads().eraseTerminal(u.id, u.generation, now);
        if (!erased) return erased;
    }
    return tx.commit();
}
}  // namespace anof
