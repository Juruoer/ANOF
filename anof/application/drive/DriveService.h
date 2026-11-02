#pragma once
#include <anof/application/drive/DriveCommands.h>
#include <anof/application/ports/DriveIdGenerator.h>
#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/application/ports/NodeIdGenerator.h>
namespace anof {
// 在metadata worker调用；所有授权、修改、计费和审计共用一个短事务，不访问磁盘。
class DriveService {
public:
    DriveService(DriveMetadataStore& store, Clock& clock, NodeIdGenerator& nodes, DriveIdGenerator& ids)
        : store_(store), clock_(clock), nodes_(nodes), ids_(ids) {}
    Result<NodeDetails> execute(const RequestContext&, const UpdateNodeCommand&);
    Result<NodeDetails> execute(const RequestContext&, const CopyFileCommand&);
    Result<TrashView> execute(const RequestContext&, const TrashNodeCommand&);
    Result<NodeDetails> execute(const RequestContext&, const RestoreTrashCommand&);
    Result<void> execute(const RequestContext&, const PurgeTrashCommand&);
    Result<TrashView> execute(const RequestContext&, const GetTrashCommand&);
    Result<Page<NodeDetails, SearchCursor>> execute(const RequestContext&, const SearchNodesCommand&);
    Result<Page<TrashView, TrashCursor>> execute(const RequestContext&, const ListTrashCommand&);

private:
    DriveMetadataStore& store_;
    Clock& clock_;
    NodeIdGenerator& nodes_;
    DriveIdGenerator& ids_;
};
}  // namespace anof
