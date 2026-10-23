#pragma once
#include <anof/application/drive/DriveCommands.h>
#include <anof/application/ports/TaskDispatcher.h>
namespace anof {
// owner loop接纳；同步拒绝无回调，接受后延后恰一次完成；命令自有，不借HTTP内存。
class DriveDispatcher {
public:
    virtual ~DriveDispatcher() = default;
    virtual TaskSubmission submit(RequestContext, UpdateNodeCommand, Completion<NodeDetails>) = 0;
    virtual TaskSubmission submit(RequestContext, CopyFileCommand, Completion<NodeDetails>) = 0;
    virtual TaskSubmission submit(RequestContext, TrashNodeCommand, Completion<TrashView>) = 0;
    virtual TaskSubmission submit(RequestContext, RestoreTrashCommand, Completion<NodeDetails>) = 0;
    virtual TaskSubmission submit(RequestContext, PurgeTrashCommand, Completion<void>) = 0;
    virtual TaskSubmission submit(RequestContext, GetTrashCommand, Completion<TrashView>) = 0;
    virtual TaskSubmission submit(RequestContext, SearchNodesCommand, Completion<Page<NodeDetails, SearchCursor>>) = 0;
    virtual TaskSubmission submit(RequestContext, ListTrashCommand, Completion<Page<TrashView, TrashCursor>>) = 0;
};
}  // namespace anof
