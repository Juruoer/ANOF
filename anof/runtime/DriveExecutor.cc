#include <anof/runtime/DriveExecutor.h>
#include <anof/application/drive/DriveService.h>
#include <muduox/net/EventLoop.h>
#include <stdexcept>
#include <type_traits>
namespace anof::runtime {
DriveExecutor::DriveExecutor(muduox::net::EventLoop& loop, MetadataExecutor& metadata, std::size_t limit)
    : loop_(loop), metadata_(metadata), limit_(limit) {
    loop_.assertInLoopThread();
    if (!limit || limit > 128) throw std::invalid_argument("Invalid drive capacity");
}
DriveExecutor::~DriveExecutor() { loop_.assertInLoopThread(); if (outstanding_) std::terminate(); }
void DriveExecutor::requestStop() { loop_.assertInLoopThread(); stopping_ = true; }
bool DriveExecutor::stopped() const { loop_.assertInLoopThread(); return stopping_ && !outstanding_; }
std::size_t DriveExecutor::outstanding() const { loop_.assertInLoopThread(); return outstanding_; }
template<class C, class T> TaskSubmission DriveExecutor::enqueue(RequestContext context, C command, Completion<T> callback) {
    loop_.assertInLoopThread();
    if (stopping_) return SubmitError::Stopping;
    if (!callback) return SubmitError::InternalError;
    if (outstanding_ >= limit_) return SubmitError::QueueFull;
    constexpr bool read = std::is_same_v<C, GetTrashCommand> || std::is_same_v<C, ListTrashCommand> || std::is_same_v<C, SearchNodesCommand>;
    try {
        auto result = std::make_shared<Result<T>>(ErrorCode::InternalError);
        auto submitted = metadata_.submitOperation(
            [context = std::move(context), command = std::move(command), result](MetadataWorkerResources& resources) -> Result<void> {
                auto* store = dynamic_cast<DriveMetadataStore*>(resources.metadata.get());
                if (!store || !resources.clock || !resources.nodeIds || !resources.driveIds) return ErrorCode::InternalError;
                DriveService service(*store, *resources.clock, *resources.nodeIds, *resources.driveIds);
                *result = service.execute(context, command);
                return {};
            },
            [this, result, callback = std::move(callback)](TaskCompletion<void> completion) mutable {
                // 包含回调执行期；异常也必须归还名额。metadata负责恰一次完成与异常隔离。
                struct Retire { std::size_t& count; ~Retire() { --count; } } retire{outstanding_};
                if (std::holds_alternative<CancelledBeforeStart>(completion.outcome))
                    callback({completion.taskId, CancelledBeforeStart{}});
                else {
                    auto& status = std::get<Result<void>>(completion.outcome);
                    if (!status) *result = status.error();
                    callback({completion.taskId, std::move(*result)});
                }
            }, read ? ErrorCode::InternalError : ErrorCode::CommitOutcomeUnknown);
        if (std::holds_alternative<AcceptedTask>(submitted)) ++outstanding_;
        return submitted;
    } catch (const std::bad_alloc&) { return SubmitError::InternalError; }
}
TaskSubmission DriveExecutor::submit(RequestContext c, UpdateNodeCommand cmd, Completion<NodeDetails> cb) { return enqueue(std::move(c), std::move(cmd), std::move(cb)); }
TaskSubmission DriveExecutor::submit(RequestContext c, CopyFileCommand cmd, Completion<NodeDetails> cb) { return enqueue(std::move(c), std::move(cmd), std::move(cb)); }
TaskSubmission DriveExecutor::submit(RequestContext c, TrashNodeCommand cmd, Completion<TrashView> cb) { return enqueue(std::move(c), std::move(cmd), std::move(cb)); }
TaskSubmission DriveExecutor::submit(RequestContext c, RestoreTrashCommand cmd, Completion<NodeDetails> cb) { return enqueue(std::move(c), std::move(cmd), std::move(cb)); }
TaskSubmission DriveExecutor::submit(RequestContext c, PurgeTrashCommand cmd, Completion<void> cb) { return enqueue(std::move(c), std::move(cmd), std::move(cb)); }
TaskSubmission DriveExecutor::submit(RequestContext c, GetTrashCommand cmd, Completion<TrashView> cb) { return enqueue(std::move(c), std::move(cmd), std::move(cb)); }
TaskSubmission DriveExecutor::submit(RequestContext c, SearchNodesCommand cmd, Completion<Page<NodeDetails, SearchCursor>> cb) { return enqueue(std::move(c), std::move(cmd), std::move(cb)); }
TaskSubmission DriveExecutor::submit(RequestContext c, ListTrashCommand cmd, Completion<Page<TrashView, TrashCursor>> cb) { return enqueue(std::move(c), std::move(cmd), std::move(cb)); }
} // namespace anof::runtime
