#pragma once

#include <anof/application/ports/TransferMetadataStore.h>
#include <anof/domain/drive/Space.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>

namespace anof::infrastructure::sqlite {

class SqliteNodeRepository final : public FileNodeRepository {
public:
    explicit SqliteNodeRepository(SqliteTransactionContext& context) : context_(context) {}
    Result<std::optional<NodeDetails>> find(NodeId) override;
    Result<Page<NodeDetails, ChildPosition>> listChildren(SpaceId, NodeId, PageSize, std::optional<ChildPosition>) override;
    Result<NodeDetails> insertFile(const NewFile&) override;
    Result<void> insertRoot(const RootDirectory&) override;
    Result<DirectoryPath> findActiveDirectoryPath(SpaceId spaceId, NodeId parentId) override;
    Result<DirectorySnapshot> insertDirectory(const NewDirectory& directory) override;
    Result<Revision> advanceRevision(NodeId id, Revision expected, UtcTime now) override;

private:
    Result<DirectoryPath> readPath(const Space& space, NodeId parentId);
    SqliteTransactionContext& context_;
};

}  // namespace anof::infrastructure::sqlite
