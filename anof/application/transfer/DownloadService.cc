#include <anof/application/transfer/DownloadService.h>
#include <anof/domain/drive/DirectoryPolicy.h>
namespace anof {
Result<DownloadGrant> prepareDownload(TransferMetadataStore& store, Clock& clock, const RequestContext& context, NodeId id) {
    auto opened = store.beginTransferRead();
    if (!opened) return opened.error();
    auto& read = *opened.value();
    auto actor = read.authorization().requireActiveSession(context.credential, clock.now());
    if (!actor) return actor.error();
    auto node = read.nodes().find(id);
    if (!node) return node.error();
    if (!node.value()) return ErrorCode::NotFound;
    auto space = read.spaces().find(node.value()->metadata.spaceId);
    if (!space) return space.error();
    if (!space.value()) return ErrorCode::IntegrityError;
    auto allowed = requireCreatePermission(actor.value(), *space.value());
    if (!allowed) return allowed.error();
    const bool directory = std::holds_alternative<DirectoryContent>(node.value()->content);
    auto parent = directory ? std::optional<NodeId>{ id } : node.value()->metadata.parentId;
    if (!parent) return ErrorCode::IntegrityError;
    auto path = read.nodes().findActiveDirectoryPath(space.value()->id, *parent);
    if (!path) return path.error();
    if (path.value().empty() || path.value().front().id != space.value()->rootNodeId || path.value().back().id != *parent)
        return ErrorCode::IntegrityError;
    if (directory) return ErrorCode::NotAFile;
    auto blob = read.blobs().findByNode(id);
    if (!blob) return blob.error();
    if (!blob.value()) return ErrorCode::IntegrityError;
    const auto& content = std::get<FileContentMetadata>(node.value()->content);
    if (content.size.value() != blob.value()->size.value() || content.sha256 != blob.value()->sha256.bytes())
        return ErrorCode::IntegrityError;
    return DownloadGrant{ actor.value().userId, { id, node.value()->metadata.name, *blob.value() } };
}
}  // namespace anof
