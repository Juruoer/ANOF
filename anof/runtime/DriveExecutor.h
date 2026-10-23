#pragma once
#include <anof/application/ports/DriveDispatcher.h>
#include <anof/runtime/MetadataExecutor.h>
namespace anof::runtime {
// 单owner loop；复用metadata队列，不增加等待队列。析构前必须等metadata完成退休。
class DriveExecutor final : public DriveDispatcher {
public:
    DriveExecutor(muduox::net::EventLoop&, MetadataExecutor&, std::size_t maxInFlight = 128);
    ~DriveExecutor() override;
    DriveExecutor(const DriveExecutor&) = delete;
    DriveExecutor& operator=(const DriveExecutor&) = delete;
    void requestStop();
    bool stopped() const;
    std::size_t outstanding() const;
    TaskSubmission submit(RequestContext, UpdateNodeCommand, Completion<NodeDetails>) override;
    TaskSubmission submit(RequestContext, CopyFileCommand, Completion<NodeDetails>) override;
    TaskSubmission submit(RequestContext, TrashNodeCommand, Completion<TrashView>) override;
    TaskSubmission submit(RequestContext, RestoreTrashCommand, Completion<NodeDetails>) override;
    TaskSubmission submit(RequestContext, PurgeTrashCommand, Completion<void>) override;
    TaskSubmission submit(RequestContext, GetTrashCommand, Completion<TrashView>) override;
    TaskSubmission submit(RequestContext, SearchNodesCommand, Completion<Page<NodeDetails, SearchCursor>>) override;
    TaskSubmission submit(RequestContext, ListTrashCommand, Completion<Page<TrashView, TrashCursor>>) override;
private:
    template<class C, class T> TaskSubmission enqueue(RequestContext, C, Completion<T>);
    muduox::net::EventLoop& loop_;
    MetadataExecutor& metadata_;
    const std::size_t limit_;
    std::size_t outstanding_ = 0;
    bool stopping_ = false;
};
} // namespace anof::runtime
