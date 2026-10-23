#include <anof/runtime/MaintenanceExecutor.h>
#include <anof/application/drive/MaintenanceService.h>
#include <anof/application/transfer/TransferRecovery.h>
#include <muduox/net/EventLoop.h>
namespace anof::runtime {
namespace {
Result<void> completed(TaskCompletion<void> c) {
    if (auto* r = std::get_if<Result<void>>(&c.outcome)) return *r;
    return ErrorCode::Retryable;
}
Result<DriveMetadataStore*> store(MetadataWorkerResources& r) {
    auto* s = dynamic_cast<DriveMetadataStore*>(r.metadata.get());
    if (!s || !r.clock) return ErrorCode::InternalError;
    return s;
}
Result<void> fresh(MetadataWorkerResources& r) {
    if (!r.reopenTransfer) return ErrorCode::IntegrityError;
    auto s = r.reopenTransfer(); if (!s) return s.error();
    if (!s.value() || !dynamic_cast<DriveMetadataStore*>(s.value().get())) return ErrorCode::IntegrityError;
    r.metadata = std::move(s).value(); return {};
}
// 每个Blob/Trash强类型ID均为随机UUID；维护请求关联同一对象，不作为幂等键。
template<class Id> RequestId request(Id id) { return RequestId::parse(id.toString()).value(); }
bool same(const BlobRecord& a, const BlobRecord& b) {
    return a.content.id == b.content.id && a.content.size == b.content.size && a.content.sha256 == b.content.sha256;
}
}
struct MaintenanceExecutor::Impl : std::enable_shared_from_this<Impl> {
    muduox::net::EventLoop& loop;
    MetadataExecutor& metadata;
    DiskExecutor& disk;
    BlobAccessRegistry& access;
    bool working = false, stopping = false, halted = false, recovering = false, started = false, deferred = false;
    std::function<void()> onStopped;
    std::function<void(Result<void>)> onRecovered;
    std::optional<TrashExpiryPosition> trashAfter;
    std::optional<BlobGcPosition> gcAfter;
    std::optional<BlobId> deletingAfter;
    std::unique_ptr<BlobAccessRegistry::Lease> blocked;
    Impl(muduox::net::EventLoop& l, MetadataExecutor& m, DiskExecutor& d, BlobAccessRegistry& a)
        : loop(l), metadata(m), disk(d), access(a) {}
    void stopped() { if (stopping && !working) { auto cb = std::move(onStopped); if (cb) cb(); } }
    void end(Result<void> result = {}) {
        working = false;
        if (!result && result.error().code == ErrorCode::IntegrityError) halted = true;
        if (recovering) { recovering = false; auto cb = std::move(onRecovered); if (cb) cb(stopping ? Result<void>{ErrorCode::Retryable} : result); }
        stopped();
    }
    void meta(MetadataExecutor::Operation op, std::function<void(Result<void>)> cb) {
        auto self = shared_from_this();
        // 先让上个完成项退休，容量1亦可推进；普通队列满只延后到下一tick。
        loop.queueInLoop([self, op = std::move(op), cb = std::move(cb)] {
            auto submitted = self->metadata.submitOperation(op, [cb](auto c) { cb(completed(std::move(c))); });
            if (auto* error = std::get_if<SubmitError>(&submitted)) {
                if (*error == SubmitError::QueueFull && self->recovering && !self->stopping)
                    self->loop.runAfter(.01, [self, op, cb] { self->meta(op, cb); });
                else { self->deferred = true; cb(ErrorCode::Retryable); }
            }
        });
    }
    void periodic() {
        auto weak = weak_from_this();
        loop.runAfter(60, [weak] { if (auto self = weak.lock()) { if (self->stopping) return; self->tick(); self->periodic(); } });
    }
    void tick() {
        if (working || stopping || halted) return;
        working = true; deferred = false;
        auto self = shared_from_this();
        auto page = std::make_shared<Result<Page<TrashEntry, TrashExpiryPosition>>>(ErrorCode::InternalError);
        meta([self, page](auto& r) -> Result<void> {
            auto s = store(r); if (!s) return s.error();
            auto read = s.value()->beginDriveRead(); if (!read) return read.error();
            *page = read.value()->trash().expired(r.clock->now(), PageSize::from(16).value(), self->trashAfter);
            return *page ? Result<void>{} : Result<void>{page->error()};
        }, [self, page](auto result) {
            if (!result) { self->end(result); return; }
            self->trashAfter = page->value().next;
            self->expirePage(std::make_shared<std::vector<TrashEntry>>(std::move(page->value().items)), 0);
        });
    }
    void expirePage(std::shared_ptr<std::vector<TrashEntry>> rows, std::size_t index) {
        if (stopping) { end(); return; }
        if (index == rows->size()) { deletingPage(); return; }
        auto self = shared_from_this(); auto row = (*rows)[index];
        meta([row](auto& r) -> Result<void> {
            auto s = store(r); if (!s) return s.error();
            auto result = MaintenanceService(*s.value(), *r.clock).expire({row.id, row.revision}, request(row.id));
            return result;
        }, [self, rows, index](auto result) {
            if (!result && result.error().code == ErrorCode::CommitOutcomeUnknown) {
                self->meta([rows, index](auto& r) -> Result<void> {
                    auto opened = fresh(r); if (!opened) return ErrorCode::IntegrityError;
                    auto s = store(r); if (!s) return s.error();
                    auto read = s.value()->beginDriveRead(); if (!read) return ErrorCode::IntegrityError;
                    auto entry = read.value()->trash().find((*rows)[index].id);
                    if (!entry) return ErrorCode::IntegrityError;
                    return {};
                }, [self, rows, index](auto checked) {
                    if (!checked) { self->halted = true; self->end(checked); }
                    else self->expirePage(rows, index + 1);
                });
                return;
            }
            if (!result && (result.error().code == ErrorCode::IntegrityError || result.error().code == ErrorCode::Retryable)) { self->end(result); return; }
            self->expirePage(rows, index + 1);
        });
    }
    void deletingPage() {
        if (stopping) { end(); return; }
        auto self = shared_from_this();
        auto page = std::make_shared<Result<Page<BlobRecord, BlobId>>>(ErrorCode::InternalError);
        meta([self, page](auto& r) -> Result<void> {
            auto s = store(r); if (!s) return s.error(); auto read = s.value()->beginDriveRead(); if (!read) return read.error();
            *page = read.value()->blobLifecycle().deleting(PageSize::from(16).value(), self->deletingAfter);
            return *page ? Result<void>{} : Result<void>{page->error()};
        }, [self, page](auto result) {
            if (!result) { self->end(result); return; }
            self->deletingAfter = page->value().next;
            self->blobs(std::make_shared<std::vector<BlobRecord>>(std::move(page->value().items)), 0, true);
        });
    }
    void candidates() {
        if (stopping) { end(); return; }
        auto self = shared_from_this(); auto page = std::make_shared<Result<Page<BlobRecord, BlobGcPosition>>>(ErrorCode::InternalError);
        meta([self, page](auto& r) -> Result<void> {
            auto s = store(r); if (!s) return s.error(); auto read = s.value()->beginDriveRead(); if (!read) return read.error();
            *page = read.value()->blobLifecycle().gcCandidates(r.clock->now(), PageSize::from(16).value(), self->gcAfter);
            return *page ? Result<void>{} : Result<void>{page->error()};
        }, [self, page](auto result) {
            if (!result) { self->end(result); return; }
            self->gcAfter = page->value().next;
            self->blobs(std::make_shared<std::vector<BlobRecord>>(std::move(page->value().items)), 0, false);
        });
    }
    struct Chain {
        BlobRecord row;
        std::unique_ptr<BlobAccessRegistry::Lease> lease;
        std::function<void(Result<void>)> done;
    };
    void blobs(std::shared_ptr<std::vector<BlobRecord>> rows, std::size_t index, bool retry) {
        if (stopping) { end(); return; }
        if (index == rows->size()) {
            if (!retry) { end(); return; }
            if (recovering) { if (deletingAfter) deletingPage(); else {
                auto self = shared_from_this();
                meta([](auto& r) -> Result<void> { auto s = store(r); if (!s) return s.error(); return auditTransferMetadata(*s.value(), r.clock->now()); },
                    [self](auto result) { self->end(result); });
            } }
            else candidates();
            return;
        }
        auto self = shared_from_this(); auto lease = access.tryAcquireDelete((*rows)[index].content.id);
        if (!lease) { loop.queueInLoop([self, rows, index, retry] { self->blobs(rows, index + 1, retry); }); return; }
        auto chain = std::make_shared<Chain>(Chain{(*rows)[index], std::move(lease), {}});
        chain->done = [self, rows, index, retry](auto result) {
            if (self->deferred || self->halted || (!result && result.error().code == ErrorCode::IntegrityError)) { self->end(result); return; }
            self->blobs(rows, index + 1, retry);
        };
        if (retry) erase(chain); else mark(chain);
    }
    void complete(std::shared_ptr<Chain> chain, Result<void> result, bool uncertain = false) {
        if (uncertain) { halted = true; blocked = std::move(chain->lease); }
        else chain->lease.reset();
        auto cb = std::move(chain->done); cb(result);
    }
    void mark(std::shared_ptr<Chain> c) {
        auto self = shared_from_this(); auto row = std::make_shared<Result<BlobRecord>>(ErrorCode::InternalError);
        meta([c, row](auto& r) -> Result<void> {
            auto s = store(r); if (!s) return s.error();
            *row = MaintenanceService(*s.value(), *r.clock).mark(c->row.content.id, c->row.revision, request(c->row.content.id));
            return *row ? Result<void>{} : Result<void>{row->error()};
        }, [self, c, row](auto result) {
            if (!result) { if (result.error().code == ErrorCode::CommitOutcomeUnknown) self->resolve(c, false); else self->complete(c, result); return; }
            c->row = row->value(); self->erase(c);
        });
    }
    void erase(std::shared_ptr<Chain> c) {
        if (stopping) { complete(c, {}); return; } // 已知持久意图，下一次启动继续。
        auto self = shared_from_this();
        auto submitted = disk.submit([c](auto& fs) -> Result<void> {
            auto opened = fs.open(ObjectFile{c->row.content.id}, false);
            if (auto* error = std::get_if<TransferFileError>(&opened)) {
                if (*error != TransferFileError::NotFound)
                    return *error == TransferFileError::Integrity || *error == TransferFileError::AlreadyExists
                        ? ErrorCode::IntegrityError : ErrorCode::Retryable;
            } else {
                auto handle = std::move(std::get<OwnedTransferFile>(opened));
                auto stat = fs.stat(*handle);
                if (auto* statError = std::get_if<TransferFileError>(&stat))
                    return *statError == TransferFileError::Integrity ? ErrorCode::IntegrityError : ErrorCode::Retryable;
                if (std::get<TransferFileStat>(stat).size.value() != c->row.content.size.value()) return ErrorCode::IntegrityError;
            }
            auto result = fs.removeAndSync(ObjectFile{c->row.content.id});
            if (auto* error = std::get_if<TransferFileError>(&result))
                return *error == TransferFileError::Integrity || *error == TransferFileError::AlreadyExists || *error == TransferFileError::NotFound
                    ? ErrorCode::IntegrityError : ErrorCode::Retryable;
            return {};
        }, [self, c](auto result) {
            auto r = completed(std::move(result)); if (!r) { self->complete(c, r); return; }
            self->finalize(c);
        }, true);
        if (!std::holds_alternative<AcceptedTask>(submitted)) loop.queueInLoop([self, c] { self->deferred = true; self->complete(c, ErrorCode::Retryable); });
    }
    void finalize(std::shared_ptr<Chain> c) {
        auto self = shared_from_this();
        meta([c](auto& r) -> Result<void> {
            auto s = store(r); if (!s) return s.error();
            return MaintenanceService(*s.value(), *r.clock).finalize(c->row.content.id, c->row.revision, request(c->row.content.id));
        }, [self, c](auto result) {
            if (!result && result.error().code == ErrorCode::CommitOutcomeUnknown) self->resolve(c, true);
            else self->complete(c, result);
        });
    }
    void resolve(std::shared_ptr<Chain> c, bool final) {
        auto self = shared_from_this(); auto row = std::make_shared<Result<std::optional<BlobRecord>>>(ErrorCode::InternalError);
        meta([c, row](auto& r) -> Result<void> {
            auto opened = fresh(r); if (!opened) return opened;
            auto s = store(r); if (!s) return s.error(); auto read = s.value()->beginDriveRead(); if (!read) return read.error();
            *row = read.value()->blobLifecycle().record(c->row.content.id);
            return *row ? Result<void>{} : Result<void>{row->error()};
        }, [self, c, row, final](auto result) {
            if (!result) { self->complete(c, ErrorCode::IntegrityError, true); return; }
            if (!row->value()) { self->complete(c, final ? Result<void>{} : Result<void>{ErrorCode::IntegrityError}, !final); return; }
            auto b = *row->value();
            const auto expected = final ? Result<Revision>{c->row.revision} : c->row.revision.add(1);
            if (same(b, c->row) && b.state == BlobState::Deleting && expected && b.revision == expected.value()) {
                c->row = b;
                if (final) self->complete(c, ErrorCode::Retryable); // 下个tick重新同步，绝不重复盲扣。
                else self->erase(c);
            } else if (!final && same(b, c->row) && b.state == BlobState::Available && b.revision == c->row.revision)
                self->complete(c, ErrorCode::Conflict);
            else self->complete(c, ErrorCode::IntegrityError, true);
        });
    }
};
MaintenanceExecutor::MaintenanceExecutor(muduox::net::EventLoop& l, MetadataExecutor& m, DiskExecutor& d, BlobAccessRegistry& a)
    : impl_(std::make_shared<Impl>(l,m,d,a)) {}
MaintenanceExecutor::~MaintenanceExecutor() { impl_->loop.assertInLoopThread(); if (impl_->working) std::terminate(); }
void MaintenanceExecutor::recover(std::function<void(Result<void>)> cb) {
    auto s = impl_; s->loop.assertInLoopThread();
    if (s->working || s->stopping || s->started || s->halted) { s->loop.queueInLoop([cb] { cb(ErrorCode::InvalidArgument); }); return; }
    s->working = s->recovering = true; s->onRecovered = std::move(cb); s->deletingAfter.reset(); s->deletingPage();
}
void MaintenanceExecutor::start() { auto s = impl_; s->loop.assertInLoopThread(); if (!s->started && !s->stopping) { s->started = true; s->periodic(); } }
void MaintenanceExecutor::tick() { impl_->loop.assertInLoopThread(); impl_->tick(); }
void MaintenanceExecutor::requestStop(std::function<void()> cb) { impl_->loop.assertInLoopThread(); impl_->stopping = true; impl_->onStopped = std::move(cb); impl_->stopped(); }
bool MaintenanceExecutor::stopped() const { return impl_->stopping && !impl_->working; }
bool MaintenanceExecutor::busy() const { return impl_->working; }
bool MaintenanceExecutor::paused() const { return impl_->halted; }
}
