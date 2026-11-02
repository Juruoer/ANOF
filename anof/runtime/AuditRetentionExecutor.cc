#include <anof/runtime/AuditRetentionExecutor.h>
#include <muduox/net/EventLoop.h>

#include <limits>
namespace anof::runtime {
struct AuditRetentionExecutor::Impl : std::enable_shared_from_this<Impl> {
    muduox::net::EventLoop& loop;
    MetadataExecutor& metadata;
    AuditRetentionDays days;
    AuditBatchSize batch;
    bool started = false, stopping = false, working = false, halted = false;
    std::function<void()> onStopped;
    Impl(muduox::net::EventLoop& l, MetadataExecutor& m, AuditRetentionDays d, AuditBatchSize b) : loop(l), metadata(m), days(d), batch(b) {}
    void done() {
        if (stopping && !working) {
            auto cb = std::move(onStopped);
            if (cb) cb();
        }
    }
    void periodic() {
        auto weak = weak_from_this();
        loop.runAfter(60, [weak] {if(auto self=weak.lock()){if(self->stopping)return;self->tick();self->periodic();} });
    }
    void tick() {
        loop.assertInLoopThread();
        if (stopping || working || halted) return;
        working = true;
        auto self = shared_from_this();
        auto task = metadata.submitOperation([days = days, batch = batch](MetadataWorkerResources& r) -> Result<void> {
   if(!r.auditRetention||!r.clock)return ErrorCode::InternalError;
   const auto now=r.clock->now();const auto span=days.value()*86400*1000000;
   if(now.time_since_epoch().count()>=std::numeric_limits<std::int64_t>::min()+span){auto result=r.auditRetention->eraseExpired({UtcTime(std::chrono::microseconds(now.time_since_epoch().count()-span)),batch});if(!result)return result.error();}
   auto runs=r.auditRetention->eraseCompletedRuns(now,batch);return runs?Result<void>{}:Result<void>{runs.error()}; }, [self](TaskCompletion<void> c) {self->working=false;if(auto r=std::get_if<Result<void>>(&c.outcome)){if(!*r&&r->error().code!=ErrorCode::Retryable)self->halted=true;}self->done(); });
        if (std::holds_alternative<SubmitError>(task)) {
            working = false;
            done();
        }
    }
};
AuditRetentionExecutor::AuditRetentionExecutor(muduox::net::EventLoop& l, MetadataExecutor& m, AuditRetentionDays d, AuditBatchSize b) : impl_(std::make_shared<Impl>(l, m, d, b)) {}
AuditRetentionExecutor::~AuditRetentionExecutor() {
    if (impl_->working) std::terminate();
}
void AuditRetentionExecutor::start() {
    impl_->loop.assertInLoopThread();
    if (impl_->started || impl_->stopping) return;
    impl_->started = true;
    impl_->periodic();
}
void AuditRetentionExecutor::tick() { impl_->tick(); }
void AuditRetentionExecutor::requestStop(std::function<void()> cb) {
    impl_->loop.assertInLoopThread();
    if (impl_->stopping) return;
    impl_->stopping = true;
    impl_->onStopped = std::move(cb);
    impl_->done();
}
bool AuditRetentionExecutor::stopped() const { return impl_->stopping && !impl_->working; }
bool AuditRetentionExecutor::paused() const { return impl_->halted; }
}  // namespace anof::runtime
