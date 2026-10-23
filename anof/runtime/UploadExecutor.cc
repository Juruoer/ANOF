#include <anof/application/transfer/TransferRecovery.h>
#include <anof/application/transfer/UploadService.h>
#include <anof/application/transfer/DownloadService.h>
#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/runtime/UploadExecutor.h>
#include <muduox/net/EventLoop.h>

#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <utility>

namespace anof::runtime {
namespace {
ErrorCode fileError(TransferFileError error) {
    switch (error) {
    case TransferFileError::NotFound:
        return ErrorCode::NotFound;
    case TransferFileError::AlreadyExists:
        return ErrorCode::IntegrityError;
    case TransferFileError::NoSpace:
        return ErrorCode::StorageFull;
    case TransferFileError::Transient:
        return ErrorCode::Retryable;
    case TransferFileError::Integrity:
        return ErrorCode::IntegrityError;
    case TransferFileError::UnknownEffect:
        return ErrorCode::CommitOutcomeUnknown;
    }
    return ErrorCode::InternalError;
}
template <class T>
Result<T> unwrap(FileResult<T> result) {
    if (auto* error = std::get_if<TransferFileError>(&result)) return fileError(*error);
    return std::move(std::get<T>(result));
}
Result<void> status(FileStatus result) {
    if (auto* error = std::get_if<TransferFileError>(&result)) return fileError(*error);
    return {};
}
Result<void> outcome(TaskCompletion<void> completion) {
    if (auto* result = std::get_if<Result<void>>(&completion.outcome)) return *result;
    return ErrorCode::Retryable;
}
enum class Phase { Queued,
                   Cancelled,
                   Running,
                   Terminal };
struct Control {
    Phase phase = Phase::Queued;
    std::uint64_t id;
    explicit Control(std::uint64_t n) : id(n) {}
};
class Handle final : public TaskHandle {
    std::shared_ptr<Control> c_;

public:
    explicit Handle(std::shared_ptr<Control> c) : c_(std::move(c)) {}
    std::uint64_t id() const noexcept override { return c_->id; }
    CancelBeforeStartResult cancelBeforeStart() noexcept override {
        if (c_->phase == Phase::Queued) {
            c_->phase = Phase::Cancelled;
            return CancelBeforeStartResult::Cancelled;
        }
        return c_->phase == Phase::Running ? CancelBeforeStartResult::AlreadyRunning
                                           : CancelBeforeStartResult::AlreadyTerminal;
    }
};
template <class State>
void watchProgress(muduox::net::EventLoop& loop, std::weak_ptr<State> weak, std::function<void()> cancel) {
    loop.runAfter(1, [&loop, weak, cancel] {
        auto state = weak.lock();
        if (!state || state->retired) return;
        if (std::chrono::steady_clock::now() - state->progress >= std::chrono::seconds(120))
            cancel();
        else
            watchProgress(loop, weak, cancel);
    });
}
Result<UploadService> service(MetadataWorkerResources& r) {
    auto* store = dynamic_cast<TransferMetadataStore*>(r.metadata.get());
    if (!store || !r.transferIds) return ErrorCode::InternalError;
    return UploadService(*store, *r.clock, *r.transferIds, *r.nodeIds);
}
Result<void> reopen(MetadataWorkerResources& r) {
    if (!r.reopenTransfer) return ErrorCode::IntegrityError;
    auto fresh = r.reopenTransfer();
    if (!fresh) return fresh.error();
    if (!fresh.value()) return ErrorCode::IntegrityError;
    r.metadata = std::move(fresh).value();
    return {};
}
}  // namespace
struct UploadExecutor::Impl : std::enable_shared_from_this<Impl> {
    muduox::net::EventLoop& loop;
    MetadataExecutor& metadata;
    DiskExecutor& disk;
    DiskSafetyPolicy safety;
    BlobAccessRegistry access;
    bool stopping = false, finished = false, verification = false, recovered = false, recovering = false,
         sweeping = false;
    std::size_t commands = 0, backgrounds = 0;
    std::uint64_t nextId = 1;
    std::function<void()> onStopped;
    struct Read;
    std::map<std::uint64_t, std::shared_ptr<Read>> reads;
    std::uint64_t nextRead = 1;
    std::size_t readSlots = 0;
    std::size_t userReads(UserId) const;
    void disableReads(UserId, bool);
    struct Patch;
    std::map<UploadId, std::shared_ptr<Patch>> patches;
    std::map<UploadId, bool> busy;
    std::set<UploadId> frozen;
    std::optional<UploadPosition> cleanupAfter;
    Impl(muduox::net::EventLoop& l, MetadataExecutor& m, DiskExecutor& d, DiskSafetyPolicy s)
        : loop(l), metadata(m), disk(d), safety(s), access(l) {}
    void finish() {
        if (!stopping || finished || commands || backgrounds || !patches.empty() || !reads.empty()) return;
        finished = true;
        auto cb = std::move(onStopped);
        if (cb) cb();
    }
    // 每个链至多一个重试计时器；链总数受命令/活动上传/流上限约束。
    void meta(MetadataExecutor::Operation operation, std::function<void(Result<void>)> cb) {
        auto self = shared_from_this();
        auto submitted = metadata.submitOperation(operation, [cb](auto done) { cb(outcome(std::move(done))); });
        if (auto* error = std::get_if<SubmitError>(&submitted)) {
            if (*error == SubmitError::QueueFull)
                loop.runAfter(
                    .01, [self, operation = std::move(operation), cb = std::move(cb)] { self->meta(operation, cb); });
            else
                loop.queueInLoop([cb = std::move(cb)] { cb(ErrorCode::Retryable); });
        }
    }
    void io(DiskExecutor::Operation operation, std::function<void(Result<void>)> cb, bool cleanup = false,
            std::size_t lane = 0) {
        auto self = shared_from_this();
        auto submitted = disk.submit(operation, [cb](auto done) { cb(outcome(std::move(done))); }, cleanup, lane);
        if (auto* error = std::get_if<SubmitError>(&submitted)) {
            if (*error == SubmitError::QueueFull)
                loop.runAfter(.01, [self, operation = std::move(operation), cb = std::move(cb), cleanup, lane] {
                    self->io(operation, cb, cleanup, lane);
                });
            else
                loop.queueInLoop([cb = std::move(cb)] { cb(ErrorCode::Retryable); });
        }
    }
    template <class T, class F>
    void use(F work, std::function<void(Result<T>)> cb) {
        auto result = std::make_shared<Result<T>>(ErrorCode::InternalError);
        meta(
            [result, work](auto& r) -> Result<void> {
                auto s = service(r);
                if (!s) return s.error();
                *result = work(s.value());
                if (!*result && result->error().code == ErrorCode::CommitOutcomeUnknown) {
                    auto fresh = reopen(r);
                    if (!fresh) return fresh;
                }
                return {};
            },
            [result, cb = std::move(cb)](auto done) mutable {
                cb(done ? std::move(*result) : Result<T>{ done.error() });
            });
    }
    template <class T, class F>
    TaskSubmission command(Completion<T> callback, F work, std::function<void()> onCancel = {}) {
        loop.assertInLoopThread();
        if (stopping || !recovered) return SubmitError::Stopping;
        if (commands >= 128) return SubmitError::QueueFull;
        if (!callback || nextId == UINT64_MAX) return SubmitError::InternalError;
        try {
            auto self = shared_from_this();
            auto control = std::make_shared<Control>(nextId++);
            auto handle = std::make_unique<Handle>(control);
            loop.queueInLoop([self, control, callback = std::move(callback), work = std::move(work),
                              onCancel = std::move(onCancel)]() mutable {
                if (control->phase == Phase::Cancelled) {
                    control->phase = Phase::Terminal;
                    if (onCancel) onCancel();
                    try {
                        callback({ control->id, CancelledBeforeStart{} });
                    } catch (...) {
                    }
                    --self->commands;
                    self->finish();
                    return;
                }
                control->phase = Phase::Running;
                auto done = [self, control, callback = std::move(callback)](Result<T> result) {
                    if (control->phase == Phase::Terminal) return;
                    control->phase = Phase::Terminal;
                    try {
                        callback({ control->id, std::move(result) });
                    } catch (...) {
                    }
                    --self->commands;
                    self->finish();
                };
                try {
                    work(done);
                } catch (...) {
                    if (control->phase != Phase::Terminal) done(ErrorCode::InternalError);
                }
            });
            ++commands;
            return AcceptedTask{ std::move(handle) };
        } catch (...) {
            return SubmitError::InternalError;
        }
    }
    void release(UploadId id) {
        busy.erase(id);
        --backgrounds;
        finish();
    }
    void clean(Upload u) {
        if (!u.cleanupPending || busy.count(u.id)) return;
        busy.emplace(u.id, true);
        ++backgrounds;
        auto self = shared_from_this();
        auto key = u.location == StagingLocation::Object ? TransferFileKey{ ObjectFile{ *u.pendingBlobId } }
                                                         : TransferFileKey{ TemporaryFile{ u.id } };
        io([key](auto& fs) { return status(fs.removeAndSync(key)); },
           [self, u](auto removed) {
               if (!removed) {
                   self->release(u.id);
                   return;
               }
               self->use<Upload>([u](auto& s) { return s.confirmCleanup(u.id, u.generation); },
                                 [self, u](auto) { self->release(u.id); });
           },
           true);
    }
    void advance(Upload u, RequestId request);
    void publish(Upload u, RequestId request);
    void resolve(UploadId);
    void hash(Upload u, RequestId request);
    void stop(std::function<void()> cb);
    void recover(std::function<void(Result<void>)>);
    void recoverOne(Upload, std::function<void(Result<void>)>);
    void verifyFile(TransferFileKey, FileLength, std::optional<Sha256>, std::function<void(Result<void>)>);
    void inventory(InventoryArea, std::optional<InventoryCursor>, std::function<void(Result<void>)>);
    void recoverPage(UploadScan, std::optional<UploadPosition>, std::function<void(Result<void>)>);
    void checkBlobs(std::optional<BlobId>, std::function<void(Result<void>)>);
    void sweep();
    void sampleStorage(Result<StorageView>, std::function<void(Result<StorageView>)>);
    void periodic();
};
struct UploadExecutor::Impl::Patch : std::enable_shared_from_this<Patch> {
    std::shared_ptr<Impl> owner;
    RequestContext context;
    UploadPatch patch;
    OwnedTransferFile file;
    UploadOffset written;
    bool inFlight = false, cancelled = false, finishing = false, retired = false;
    std::chrono::steady_clock::time_point progress = std::chrono::steady_clock::now();
    Patch(std::shared_ptr<Impl> o, RequestContext c, UploadPatch p)
        : owner(std::move(o)), context(std::move(c)), patch(std::move(p)), written(patch.upload.durableOffset) {}
    void retire() {
        retired = true;
        owner->busy.erase(patch.upload.id);
        owner->patches.erase(patch.upload.id);
        owner->finish();
    }
    void close(bool trim, std::function<void(Result<void>)> cb = {}) {
        auto self = shared_from_this();
        inFlight = true;
        // 先权威重读；提交不明必须新连接。仅按数据库确认的前缀截断，失败保留现场。
        auto current = std::make_shared<Result<Upload>>(ErrorCode::InternalError);
        owner->meta(
            [self, current](auto& r) -> Result<void> {
                auto fresh = reopen(r);
                if (!fresh) return fresh;
                auto s = service(r);
                if (!s) return s.error();
                *current = s.value().read(self->patch.upload.id);
                return {};
            },
            [self, current, trim, cb](auto read) {
                self->owner->io(
                    [self, current, read, trim](auto& fs) -> Result<void> {
                        Result<void> result;
                        if (trim && read && *current && current->value().durableOffset.value() <= self->written.value())
                            result = status(fs.truncateAndSync(*self->file, current->value().durableOffset));
                        else if (trim)
                            result = ErrorCode::CommitOutcomeUnknown;
                        self->file.reset();
                        return result;
                    },
                    [self, current, cb](auto result) {
                        if (!result) self->owner->frozen.insert(self->patch.upload.id);
                        self->inFlight = false;
                        self->retire();
                        if (cb) cb(result);
                        if (*current && current->value().cleanupPending) self->owner->clean(current->value());
                    },
                    true);
            });
    }
    void cancel() {
        cancelled = true;
        if (!inFlight && !retired) close(true);
    }
    void watch() {
        auto weak = weak_from_this();
        owner->loop.runAfter(1, [weak] {
            if (auto self = weak.lock()) {
                if (self->retired) return;
                if (std::chrono::steady_clock::now() - self->progress >= std::chrono::seconds(120))
                    self->cancel();
                else
                    self->watch();
            }
        });
    }
};
namespace {
// stream 包装器销毁只请求取消；实际租约由协调器持有到文件/事务阶段退休。
template <class State>
class Stream final : public PatchStream {
    std::shared_ptr<State> s_;

public:
    explicit Stream(std::shared_ptr<State> s) : s_(std::move(s)) {}
    ~Stream() override { s_->cancel(); }
    UploadGeneration generation() const noexcept override { return s_->patch.upload.generation; }
    void cancel() noexcept override { s_->cancel(); }
    TaskSubmission feed(OwnedTransferBlock& block, Completion<OwnedTransferBlock> cb) override {
        auto s = s_;
        if (s->cancelled || s->finishing || s->retired) return SubmitError::Stopping;
        if (s->inFlight) return SubmitError::QueueFull;
        if (!cb || !block || block->size == 0 || block->size > block->bytes.size() ||
            block->size > static_cast<std::uint64_t>(s->patch.end.value() - s->written.value()))
            return SubmitError::InternalError;
        auto held = std::make_shared<OwnedTransferBlock>();
        auto submitted = s->owner->template command<OwnedTransferBlock>(
            std::move(cb),
            [s, held](auto done) {
                if (s->cancelled) {
                    s->inFlight = false;
                    s->close(true);
                    done(ErrorCode::Retryable);
                    return;
                }
                s->owner->io([s, held](auto& fs) { return status(fs.write(*s->file, s->written, **held)); },
                             [s, held, done](auto result) mutable {
                                 s->inFlight = false;
                                 if (result) {
                                     s->written = s->written.add(static_cast<std::int64_t>((*held)->size)).value();
                                     s->progress = std::chrono::steady_clock::now();
                                 } else
                                     s->cancelled = true;
                                 if (s->cancelled) s->close(true);
                                 if (result)
                                     done(std::move(*held));
                                 else
                                     done(result.error());
                             });
            },
            [s] {
                s->inFlight = false;
                s->cancel();
            });
        if (std::holds_alternative<AcceptedTask>(submitted)) {
            *held = std::move(block);
            s->inFlight = true;
        }
        return submitted;
    }
    TaskSubmission finish(Completion<UploadOffset> cb) override {
        auto s = s_;
        if (s->cancelled || s->finishing || s->retired) return SubmitError::Stopping;
        if (s->inFlight) return SubmitError::QueueFull;
        if (s->written != s->patch.end || !cb) return SubmitError::InternalError;
        auto submitted = s->owner->template command<UploadOffset>(
            std::move(cb),
            [s](auto done) {
                s->owner->io([s](auto& fs) { return status(fs.syncData(*s->file)); },
                             [s, done](auto synced) {
                                 if (!synced || s->cancelled) {
                                     s->close(true, [done, synced](auto) {
                                         done(synced ? Error{ ErrorCode::Retryable } : synced.error());
                                     });
                                     return;
                                 }
                                 s->owner->template use<Upload>(
                                     [s](auto& service) { return service.commitPatch(s->context, s->patch); },
                                     [s, done](auto committed) {
                                         s->close(!committed, [s, done, committed](auto closed) {
                                             if (!committed)
                                                 done(committed.error());
                                             else if (!closed)
                                                 done(closed.error());
                                             else
                                                 done(s->patch.end);
                                         });
                                     });
                             });
            },
            [s] {
                s->inFlight = false;
                s->cancel();
            });
        if (std::holds_alternative<AcceptedTask>(submitted)) {
            s->finishing = true;
            s->inFlight = true;
        }
        return submitted;
    }
};
}  // namespace
void UploadExecutor::Impl::advance(Upload u, RequestId request) {
    if (stopping || busy.count(u.id) || frozen.count(u.id)) return;
    if (u.cleanupPending) {
        clean(u);
        return;
    }
    if (u.state == UploadState::Verifying) {
        if (!verification) hash(u, request);
    } else if (u.state == UploadState::Ready)
        publish(u, request);
}
void UploadExecutor::Impl::hash(Upload u, RequestId request) {
    verification = true;
    busy.emplace(u.id, true);
    ++backgrounds;
    struct HashJob {
        OwnedTransferFile file;
        OwnedTransferHash hash;
        TransferBlock block;
        UploadOffset offset;
        bool retired = false;
        std::chrono::steady_clock::time_point progress = std::chrono::steady_clock::now();
    };
    auto job = std::make_shared<HashJob>();
    auto self = shared_from_this();
    watchProgress(loop, std::weak_ptr<HashJob>(job), [weak = weak_from_this(), id = u.id] {
        if (auto owner = weak.lock()) {
            auto it = owner->busy.find(id);
            if (it != owner->busy.end()) it->second = false;
        }
    });
    auto finishHash = [self, job, u, request](Result<Sha256> digest) {
        job->retired = true;
        self->io(
            [job](auto&) -> Result<void> {
                job->file.reset();
                job->hash.reset();
                return {};
            },
            [self, u, request, digest](auto) {
                self->verification = false;
                if (!digest) {
                    self->use<Upload>(
                        [u](auto& s) { return s.read(u.id); },
                        [self, u, request](auto current) {
                            if (!current || self->stopping) {
                                self->release(u.id);
                                return;
                            }
                            auto row = current.value();
                            if (row.cleanupPending) {
                                self->release(u.id);
                                self->clean(row);
                                return;
                            }
                            self->use<Upload>(
                                [row, request](auto& service) {
                                    return service.terminate(
                                        row.id, row.generation,
                                        row.cancelRequested ? UploadState::Cancelled : UploadState::Failed,
                                        row.cancelRequested ? TransferFailure::UserDisabled
                                                            : TransferFailure::TemporarilyUnavailable,
                                        request);
                                },
                                [self, u](auto terminal) {
                                    self->release(u.id);
                                    if (terminal) self->clean(terminal.value());
                                });
                        });
                    return;
                }
                self->use<Upload>(
                    [u, request, digest](auto& s) {
                        return s.recordVerification(u.id, u.generation, digest.value(), request);
                    },
                    [self, u, request](auto updated) {
                        self->release(u.id);
                        if (updated) self->advance(updated.value(), request);
                        self->sweep();
                    });
            },
            true, 1);
    };
    auto next = std::make_shared<std::function<void()>>();
    *next = [self, job, u, next, finishHash] {
        if (self->stopping || !self->busy.at(u.id)) {
            *next = {};
            finishHash(ErrorCode::Retryable);
            return;
        }
        auto digest = std::make_shared<Result<Sha256>>(ErrorCode::InternalError);
        self->io(
            [job, u, digest](auto& fs) -> Result<void> {
                if (!job->file) {
                    auto opened = unwrap(fs.open(TemporaryFile{ u.id }, false));
                    if (!opened) return opened.error();
                    job->file = std::move(opened).value();
                    auto size = unwrap(fs.stat(*job->file));
                    if (!size) return size.error();
                    if (size.value().size.value() != u.original.length.value()) return ErrorCode::IntegrityError;
                    auto hash = unwrap(fs.beginHash());
                    if (!hash) return hash.error();
                    job->hash = std::move(hash).value();
                }
                auto count = static_cast<std::size_t>(
                    std::min<std::int64_t>(kTransferBlockBytes, u.original.length.value() - job->offset.value()));
                if (count) {
                    auto read = status(fs.read(*job->file, job->offset, count, job->block));
                    if (!read) return read;
                    auto hashed = status(fs.updateHash(*job->hash, job->block));
                    if (!hashed) return hashed;
                    job->offset = job->offset.add(static_cast<std::int64_t>(count)).value();
                }
                if (job->offset.value() == u.original.length.value())
                    *digest = unwrap(fs.finishHash(std::move(job->hash)));
                return {};
            },
            [job, u, next, finishHash, digest](auto read) {
                if (read) job->progress = std::chrono::steady_clock::now();
                if (!read || job->offset.value() == u.original.length.value()) {
                    auto cb = finishHash;
                    *next = {};
                    cb(read ? *digest : Result<Sha256>{ read.error() });
                } else {
                    auto invoke = *next;
                    invoke();
                }
            },
            false, 1);
    };
    auto invoke = *next;
    invoke();
}
void UploadExecutor::Impl::resolve(UploadId id) {
    auto self = shared_from_this();
    auto row = std::make_shared<Result<Upload>>(ErrorCode::InternalError);
    meta(
        [id, row](auto& r) -> Result<void> {
            auto fresh = reopen(r);
            if (!fresh) return fresh;
            auto s = service(r);
            if (!s) return s.error();
            *row = s.value().read(id);
            return {};
        },
        [self, id, row](auto read) {
            if (!read || !*row) {
                self->frozen.insert(id);
                self->release(id);
                return;
            }
            if (row->value().state == UploadState::Completed) {
                self->release(id);
                return;
            }
            self->recoverOne(row->value(), [self, id](auto checked) {
                if (!checked) self->frozen.insert(id);
                self->release(id);
            });
        });
}
void UploadExecutor::Impl::publish(Upload u, RequestId request) {
    busy.emplace(u.id, true);
    ++backgrounds;
    auto self = shared_from_this();
    use<Upload>([u](auto& s) { return s.preparePublish(u.id, u.generation); },
                [self, u, request](auto intent) {
                    if (!intent) {
                        self->release(u.id);
                        return;
                    }
                    auto row = intent.value();
                    self->io(
                        [row](auto& fs) {
                            return status(row.location == StagingLocation::Object
                                              ? fs.syncPublication(row.id, *row.pendingBlobId)
                                              : fs.publish(row.id, *row.pendingBlobId));
                        },
                        [self, row, request](auto moved) {
                            if (!moved) {
                                self->resolve(row.id);
                                return;
                            }
                            self->use<Upload>(
                                [row, request](auto& s) { return s.commitPublish(row.id, row.generation, request); },
                                [self, row](auto committed) {
                                    if (!committed && (committed.error().code == ErrorCode::NameConflict ||
                                                       committed.error().code == ErrorCode::NotFound ||
                                                       committed.error().code == ErrorCode::ParentNotDirectory ||
                                                       committed.error().code == ErrorCode::DepthLimitExceeded)) {
                                        auto reason = committed.error().code == ErrorCode::NameConflict
                                                          ? TransferFailure::NameConflict
                                                      : committed.error().code == ErrorCode::ParentNotDirectory
                                                          ? TransferFailure::ParentNotDirectory
                                                      : committed.error().code == ErrorCode::DepthLimitExceeded
                                                          ? TransferFailure::DepthLimitExceeded
                                                          : TransferFailure::NotFound;
                                        self->use<Upload>(
                                            [row, reason](auto& s) {
                                                return s.publicationConflict(row.id, row.generation, reason);
                                            },
                                            [self, row](auto) { self->release(row.id); });
                                    } else if (!committed)
                                        self->resolve(row.id);
                                    else
                                        self->release(row.id);
                                });
                        });
                });
}
void UploadExecutor::Impl::verifyFile(TransferFileKey key, FileLength length, std::optional<Sha256> expected,
                                      std::function<void(Result<void>)> cb) {
    struct State {
        OwnedTransferFile file;
        OwnedTransferHash hash;
        TransferBlock block;
        UploadOffset offset;
    };
    auto state = std::make_shared<State>();
    auto self = shared_from_this();
    auto next = std::make_shared<std::function<void()>>();
    *next = [self, state, key, length, expected, cb, next] {
        if (self->stopping) {
            *next = {};
            self->io(
                [state](auto&) -> Result<void> {
                    state->file.reset();
                    state->hash.reset();
                    return {};
                },
                [cb](auto) { cb(ErrorCode::Retryable); }, true, 1);
            return;
        }
        self->io(
            [state, key, length, expected](auto& fs) -> Result<void> {
                if (!state->file) {
                    auto opened = unwrap(fs.open(key, false));
                    if (!opened) return opened.error();
                    state->file = std::move(opened).value();
                    auto size = unwrap(fs.stat(*state->file));
                    if (!size) return size.error();
                    if (size.value().size.value() != length.value()) return ErrorCode::IntegrityError;
                    if (expected) {
                        auto hash = unwrap(fs.beginHash());
                        if (!hash) return hash.error();
                        state->hash = std::move(hash).value();
                    }
                }
                if (!expected) return {};
                auto count = static_cast<std::size_t>(
                    std::min<std::int64_t>(kTransferBlockBytes, length.value() - state->offset.value()));
                if (count) {
                    auto r = status(fs.read(*state->file, state->offset, count, state->block));
                    if (!r) return r;
                    r = status(fs.updateHash(*state->hash, state->block));
                    if (!r) return r;
                    state->offset = state->offset.add(static_cast<std::int64_t>(count)).value();
                }
                if (state->offset.value() == length.value()) {
                    auto hash = unwrap(fs.finishHash(std::move(state->hash)));
                    if (!hash) return hash.error();
                    if (hash.value() != *expected) return ErrorCode::IntegrityError;
                }
                return {};
            },
            [self, state, length, expected, cb, next](auto r) {
                if (!r || !expected || state->offset.value() == length.value()) {
                    *next = {};
                    self->io(
                        [state](auto&) -> Result<void> {
                            state->file.reset();
                            state->hash.reset();
                            return {};
                        },
                        [cb, r](auto) { cb(r); }, true, 1);
                } else {
                    auto invoke = *next;
                    invoke();
                }
            },
            false, 1);
    };
    auto invoke = *next;
    invoke();
}
void UploadExecutor::Impl::inventory(InventoryArea area, std::optional<InventoryCursor> after,
                                     std::function<void(Result<void>)> cb) {
    if (stopping) {
        cb(ErrorCode::Retryable);
        return;
    }
    auto self = shared_from_this();
    auto page = std::make_shared<Result<Page<InventoryEntry, InventoryCursor>>>(ErrorCode::InternalError);
    io(
        [page, area, after](auto& fs) -> Result<void> {
            *page = unwrap(fs.inventory(area, PageSize::from(64).value(), after));
            return {};
        },
        [self, area, page, cb](auto read) {
            if (!read || !*page) {
                cb(!read ? read.error() : page->error());
                return;
            }
            self->meta(
                [page, area](auto& r) -> Result<void> {
                    auto* store = dynamic_cast<TransferMetadataStore*>(r.metadata.get());
                    if (!store) return ErrorCode::InternalError;
                    auto opened = store->beginTransferRead();
                    if (!opened) return opened.error();
                    auto& view = *opened.value();
                    for (const auto& entry : page->value().items) {
                        if (area == InventoryArea::Quarantine || !entry.knownKey) return ErrorCode::IntegrityError;
                        if (auto* temp = std::get_if<TemporaryFile>(&*entry.knownKey)) {
                            auto u = view.uploads().find(temp->id);
                            if (!u) return u.error();
                            if (!u.value() || u.value()->state == UploadState::Completed ||
                                (isTerminal(u.value()->state) && !u.value()->cleanupPending) ||
                                u.value()->location == StagingLocation::Object)
                                return ErrorCode::IntegrityError;
                        } else {
                            auto id = std::get<ObjectFile>(*entry.knownKey).id;
                            auto blob = view.blobs().find(id);
                            if (!blob) return blob.error();
                            if (blob.value() && entry.size.value() != blob.value()->size.value()) return ErrorCode::IntegrityError;
                            if (!blob.value()) {
                                bool owned = false;
                                for (auto scan : { UploadScan::Active, UploadScan::CleanupPending }) {
                                    std::optional<UploadPosition> cursor;
                                    do {
                                        auto owners = view.uploads().scan(scan, r.clock->now(),
                                                                          PageSize::from(64).value(), cursor);
                                        if (!owners) return owners.error();
                                        for (const auto& u : owners.value().items)
                                            if (u.pendingBlobId && *u.pendingBlobId == id) {
                                                if (owned) return ErrorCode::IntegrityError;
                                                if (u.state != UploadState::Committing &&
                                                    u.location != StagingLocation::Object)
                                                    return ErrorCode::IntegrityError;
                                                owned = true;
                                            }
                                        cursor = owners.value().next;
                                    } while (cursor);
                                }
                                if (!owned) return ErrorCode::IntegrityError;
                            }
                        }
                    }
                    return {};
                },
                [self, area, page, cb](auto valid) {
                    if (!valid) {
                        cb(valid);
                        return;
                    }
                    if (page->value().next)
                        self->inventory(area, page->value().next, cb);
                    else if (area == InventoryArea::Uploads)
                        self->inventory(InventoryArea::Objects, {}, cb);
                    else if (area == InventoryArea::Objects)
                        self->inventory(InventoryArea::Quarantine, {}, cb);
                    else
                        cb({});
                });
        });
}
void UploadExecutor::Impl::checkBlobs(std::optional<BlobId> after, std::function<void(Result<void>)> cb) {
    if (stopping) {
        cb(ErrorCode::Retryable);
        return;
    }
    auto self = shared_from_this();
    auto page = std::make_shared<Result<Page<Blob, BlobId>>>(ErrorCode::InternalError);
    meta(
        [page, after](auto& r) -> Result<void> {
            auto* store = dynamic_cast<TransferMetadataStore*>(r.metadata.get());
            if (!store) return ErrorCode::InternalError;
            auto read = store->beginTransferRead();
            if (!read) return read.error();
            *page = read.value()->blobs().list(PageSize::from(64).value(), after);
            if (!*page) return page->error();
            auto* drive = dynamic_cast<DriveReadSnapshot*>(read.value().get());
            if (!drive) return ErrorCode::InternalError;
            std::vector<Blob> available;
            for (const auto& blob : page->value().items) {
                auto row = drive->blobLifecycle().record(blob.id);
                if (!row) return row.error();
                if (!row.value()) return ErrorCode::IntegrityError;
                if (row.value()->state == BlobState::Available) available.push_back(blob);
            }
            page->value().items = std::move(available);
            return {};
        },
        [self, page, cb](auto read) {
            if (!read || !*page) {
                cb(!read ? read.error() : page->error());
                return;
            }
            auto index = std::make_shared<std::size_t>(0);
            auto next = std::make_shared<std::function<void(Result<void>)>>();
            *next = [self, page, cb, index, next](auto valid) {
                if (!valid || *index == page->value().items.size()) {
                    *next = {};
                    if (!valid)
                        cb(valid);
                    else if (page->value().next)
                        self->checkBlobs(page->value().next, cb);
                    else
                        cb({});
                    return;
                }
                const auto b = page->value().items[(*index)++];
                self->verifyFile(ObjectFile{ b.id }, b.size, {}, [next](auto r) {
                    auto invoke = *next;
                    // AVAILABLE包含无引用宽限对象；缺文件是损坏，不能由GC补成功。
                    invoke(!r && r.error().code == ErrorCode::NotFound ? Result<void>{ErrorCode::IntegrityError} : r);
                });
            };
            auto invoke = *next;
            invoke({});
        });
}
void UploadExecutor::Impl::recoverOne(Upload u, std::function<void(Result<void>)> cb) {
    auto self = shared_from_this();
    const auto request = RequestId::parse(u.id.toString()).value();
    // COMMITTING 先确定 rename 的实际位置，再处理取消，避免删除错误 key。
    if (u.state == UploadState::Committing) {
        auto moved = std::make_shared<bool>(false);
        io(
            [u, moved](auto& fs) -> Result<void> {
                auto temp = fs.open(TemporaryFile{ u.id }, false);
                auto object = fs.open(ObjectFile{ *u.pendingBlobId }, false);
                const bool t = std::holds_alternative<OwnedTransferFile>(temp),
                           o = std::holds_alternative<OwnedTransferFile>(object);
                if ((!t && std::get<TransferFileError>(temp) != TransferFileError::NotFound) ||
                    (!o && std::get<TransferFileError>(object) != TransferFileError::NotFound) || t == o)
                    return ErrorCode::IntegrityError;
                *moved = o;
                return {};
            },
            [self, u, request, moved, cb](auto probed) {
                if (!probed) {
                    cb(probed);
                    return;
                }
                if (u.cancelRequested && !*moved) {
                    self->use<Upload>(
                        [u, request](auto& service) {
                            return service.terminate(u.id, u.generation, UploadState::Cancelled,
                                                     TransferFailure::UserDisabled, request);
                        },
                        [self, cb](auto terminal) {
                            if (!terminal)
                                cb(terminal.error());
                            else
                                self->recoverOne(terminal.value(), cb);
                        });
                    return;
                }
                auto key = *moved ? TransferFileKey{ ObjectFile{ *u.pendingBlobId } }
                                  : TransferFileKey{ TemporaryFile{ u.id } };
                self->verifyFile(key, u.original.length, u.actualSha256, [self, u, request, moved, cb](auto valid) {
                    if (!valid) {
                        cb(valid);
                        return;
                    }
                    self->io(
                        [u, moved](auto& fs) {
                            return status(*moved ? fs.syncPublication(u.id, *u.pendingBlobId)
                                                 : fs.publish(u.id, *u.pendingBlobId));
                        },
                        [self, u, request, cb](auto synced) {
                            if (!synced) {
                                cb(synced);
                                return;
                            }
                            self->use<Upload>(
                                [u](auto& s) { return s.markObject(u.id, u.generation); },
                                [self, u, request, cb](auto marked) {
                                    if (!marked) {
                                        cb(marked.error());
                                        return;
                                    }
                                    auto row = marked.value();
                                    if (row.cancelRequested) {
                                        self->use<Upload>(
                                            [row, request](auto& s) {
                                                return s.terminate(row.id, row.generation, UploadState::Cancelled,
                                                                   TransferFailure::UserDisabled, request);
                                            },
                                            [self, cb](auto terminal) {
                                                if (!terminal)
                                                    cb(terminal.error());
                                                else
                                                    self->recoverOne(terminal.value(), cb);
                                            });
                                        return;
                                    }
                                    self->use<Upload>(
                                        [row, request](auto& s) {
                                            return s.commitPublish(row.id, row.generation, request);
                                        },
                                        [self, row, cb](auto committed) {
                                            if (committed) {
                                                cb({});
                                                return;
                                            }
                                            auto code = committed.error().code;
                                            if (code == ErrorCode::NameConflict || code == ErrorCode::NotFound ||
                                                code == ErrorCode::ParentNotDirectory ||
                                                code == ErrorCode::DepthLimitExceeded) {
                                                auto reason = code == ErrorCode::NameConflict
                                                                  ? TransferFailure::NameConflict
                                                              : code == ErrorCode::NotFound ? TransferFailure::NotFound
                                                              : code == ErrorCode::ParentNotDirectory
                                                                  ? TransferFailure::ParentNotDirectory
                                                                  : TransferFailure::DepthLimitExceeded;
                                                self->use<Upload>(
                                                    [row, reason](auto& s) {
                                                        return s.publicationConflict(row.id, row.generation, reason);
                                                    },
                                                    [cb](auto changed) {
                                                        cb(changed ? Result<void>{} : Result<void>{ changed.error() });
                                                    });
                                            } else
                                                cb(committed.error());
                                        });
                                });
                        });
                });
            });
        return;
    }
    if (u.cleanupPending) {
        auto key = u.location == StagingLocation::Object ? TransferFileKey{ ObjectFile{ *u.pendingBlobId } }
                                                         : TransferFileKey{ TemporaryFile{ u.id } };
        io([key](auto& fs) { return status(fs.removeAndSync(key)); },
           [self, u, cb](auto removed) {
               if (!removed) {
                   cb(removed);
                   return;
               }
               self->use<Upload>(
                   [u](auto& s) { return s.confirmCleanup(u.id, u.generation); },
                   [cb](auto cleaned) { cb(cleaned ? Result<void>{} : Result<void>{ cleaned.error() }); });
           },
           true);
        return;
    }
    auto expired = std::make_shared<bool>(false);
    meta(
        [u, expired](auto& r) -> Result<void> {
            *expired = u.expiresAt <= r.clock->now();
            return {};
        },
        [self, u, request, expired, cb](auto read) {
            if (!read) {
                cb(read);
                return;
            }
            if (u.cancelRequested || *expired) {
                self->use<Upload>(
                    [u, request, expired](auto& s) {
                        return s.terminate(
                            u.id, u.generation, u.cancelRequested ? UploadState::Cancelled : UploadState::Expired,
                            u.cancelRequested ? TransferFailure::UserDisabled : TransferFailure::UploadExpired,
                            request);
                    },
                    [self, cb](auto terminal) {
                        if (!terminal)
                            cb(terminal.error());
                        else
                            self->recoverOne(terminal.value(), cb);
                    });
                return;
            }
            if (u.state == UploadState::Ready) {
                auto key = u.location == StagingLocation::Object ? TransferFileKey{ ObjectFile{ *u.pendingBlobId } }
                                                                 : TransferFileKey{ TemporaryFile{ u.id } };
                self->verifyFile(key, u.original.length, u.actualSha256, cb);
                return;
            }
            self->io(
                [u](auto& fs) -> Result<void> {
                    auto opened = fs.open(TemporaryFile{ u.id }, true);
                    if (auto* error = std::get_if<TransferFileError>(&opened)) {
                        if (*error != TransferFileError::NotFound || u.state != UploadState::Creating)
                            return ErrorCode::IntegrityError;
                        opened = fs.create(u.id);
                    }
                    auto file = unwrap(std::move(opened));
                    if (!file) return file.error();
                    auto size = unwrap(fs.stat(*file.value()));
                    if (!size) return size.error();
                    if ((u.state == UploadState::Creating && size.value().size.value() != 0) ||
                        size.value().size.value() < u.durableOffset.value() ||
                        (u.state == UploadState::Verifying && size.value().size.value() != u.original.length.value()))
                        return ErrorCode::IntegrityError;
                    return status(fs.truncateAndSync(*file.value(), u.durableOffset));
                },
                [self, u, cb](auto valid) {
                    if (!valid || u.state != UploadState::Creating) {
                        cb(valid);
                        return;
                    }
                    self->use<Upload>(
                        [u](auto& s) { return s.opened(u.id, u.generation); },
                        [cb](auto opened) { cb(opened ? Result<void>{} : Result<void>{ opened.error() }); });
                });
        });
}
void UploadExecutor::Impl::recoverPage(UploadScan scan, std::optional<UploadPosition> after,
                                       std::function<void(Result<void>)> cb) {
    auto self = shared_from_this();
    auto page = std::make_shared<Result<Page<Upload, UploadPosition>>>(ErrorCode::InternalError);
    meta(
        [page, scan, after](auto& r) -> Result<void> {
            auto* store = dynamic_cast<TransferMetadataStore*>(r.metadata.get());
            if (!store) return ErrorCode::InternalError;
            auto read = store->beginTransferRead();
            if (!read) return read.error();
            *page = read.value()->uploads().scan(scan, r.clock->now(), PageSize::from(64).value(), after);
            return {};
        },
        [self, page, scan, cb](auto read) {
            if (!read || !*page) {
                cb(!read ? read.error() : page->error());
                return;
            }
            auto index = std::make_shared<std::size_t>(0);
            auto next = std::make_shared<std::function<void(Result<void>)>>();
            *next = [self, page, scan, cb, index, next](auto valid) {
                if (!valid || self->stopping || *index == page->value().items.size()) {
                    *next = {};
                    if (!valid)
                        cb(valid);
                    else if (self->stopping)
                        cb(ErrorCode::Retryable);
                    else if (page->value().next)
                        self->recoverPage(scan, page->value().next, cb);
                    else if (scan == UploadScan::Active)
                        self->recoverPage(UploadScan::CleanupPending, {}, cb);
                    else
                        cb({});
                    return;
                }
                self->recoverOne(page->value().items[(*index)++], [next](auto r) {
                    auto invoke = *next;
                    invoke(r);
                });
            };
            auto invoke = *next;
            invoke({});
        });
}
void UploadExecutor::Impl::recover(std::function<void(Result<void>)> cb) {
    if (recovering || recovered || stopping) {
        loop.queueInLoop([cb] { cb(ErrorCode::InvalidArgument); });
        return;
    }
    recovering = true;
    ++backgrounds;
    auto self = shared_from_this();
    auto done = [self, cb](Result<void> result) {
        self->recovering = false;
        self->recovered = bool(result) && !self->stopping;
        --self->backgrounds;
        cb(self->stopping ? Result<void>{ ErrorCode::Retryable } : result);
        if (self->recovered) {
            self->sweep();
            self->periodic();
        }
        self->finish();
    };
    meta(
        [](auto& r) -> Result<void> {
            auto* store = dynamic_cast<TransferMetadataStore*>(r.metadata.get());
            if (!store) return ErrorCode::InternalError;
            return auditTransferMetadata(*store, r.clock->now());
        },
        [self, done](auto valid) {
            if (!valid) {
                done(valid);
                return;
            }
            self->inventory(InventoryArea::Uploads, {}, [self, done](auto inventory) {
                if (!inventory) {
                    done(inventory);
                    return;
                }
                self->checkBlobs({}, [self, done](auto objects) {
                    if (!objects) {
                        done(objects);
                        return;
                    }
                    self->recoverPage(UploadScan::Active, {}, done);
                });
            });
        });
}
void UploadExecutor::Impl::sampleStorage(Result<StorageView> view, std::function<void(Result<StorageView>)> cb) {
    if (!view) {
        cb(view.error());
        return;
    }
    auto value = std::make_shared<StorageView>(view.value());
    auto sample = std::make_shared<Result<DiskCapacity>>(ErrorCode::InternalError);
    auto self = shared_from_this();
    io(
        [sample](auto& fs) -> Result<void> {
            *sample = unwrap(fs.capacity());
            return {};
        },
        [self, sample, value, cb](auto measured) {
            if (!measured || !*sample) {
                cb(*value);
                return;
            }
            auto margin = diskSafetyMargin(sample->value().volume, self->safety);
            if (!margin) {
                cb(*value);
                return;
            }
            self->meta(
                [sample, value, margin](auto& r) -> Result<void> {
                    value->disk = DiskSample{ sample->value().free, margin.value(), r.clock->now() };
                    return {};
                },
                [value, cb](auto read) {
                    if (!read) value->disk.reset();
                    cb(*value);
                });
        });
}
void UploadExecutor::Impl::periodic() {
    auto weak = weak_from_this();
    loop.runAfter(60, [weak] {
        if (auto self = weak.lock()) {
            if (self->stopping) return;
            self->sweep();
            self->periodic();
        }
    });
}
void UploadExecutor::Impl::sweep() {
    if (stopping || !recovered || sweeping) return;
    sweeping = true;
    ++backgrounds;
    auto self = shared_from_this();
    auto rows = std::make_shared<std::vector<Upload>>();
    auto cursor = std::make_shared<std::optional<UploadPosition>>(cleanupAfter);
    auto now = std::make_shared<UtcTime>();
    meta(
        [rows, cursor, now](auto& r) -> Result<void> {
            *now = r.clock->now();
            auto* store = dynamic_cast<TransferMetadataStore*>(r.metadata.get());
            if (!store) return ErrorCode::InternalError;
            {
                auto read = store->beginTransferRead();
                if (!read) return read.error();
                for (auto scan : { UploadScan::Active, UploadScan::CleanupPending }) {
                    auto page =
                        read.value()->uploads().scan(scan, r.clock->now(), PageSize::from(64).value(),
                                                     scan == UploadScan::CleanupPending ? *cursor : std::nullopt);
                    if (!page) return page.error();
                    if (scan == UploadScan::CleanupPending) *cursor = page.value().next;
                    for (auto& u : page.value().items) rows->push_back(std::move(u));
                }
            }
            return forgetTerminalUploads(*store, r.clock->now());
        },
        [self, rows, cursor, now](auto read) {
            if (read) self->cleanupAfter = *cursor;
            if (read && !self->stopping)
                for (const auto& u : *rows) {
                    if (u.cancelRequested) {
                        auto patch = self->patches.find(u.id);
                        if (patch != self->patches.end()) patch->second->cancel();
                    }
                    if (self->frozen.count(u.id)) continue;
                    if (self->busy.count(u.id)) {
                        if (u.cancelRequested || (u.state != UploadState::Committing && u.expiresAt <= *now)) {
                            self->busy[u.id] = false;
                            auto patch = self->patches.find(u.id);
                            if (patch != self->patches.end()) patch->second->cancel();
                            if (u.state != UploadState::Committing) {
                                ++self->backgrounds;
                                self->use<Upload>(
                                    [u](auto& service) {
                                        return service.terminate(
                                            u.id, u.generation,
                                            u.cancelRequested ? UploadState::Cancelled : UploadState::Expired,
                                            u.cancelRequested ? TransferFailure::UserDisabled
                                                              : TransferFailure::UploadExpired,
                                            RequestId::parse(u.id.toString()).value());
                                    },
                                    [self](auto terminal) {
                                        if (terminal) self->clean(terminal.value());
                                        --self->backgrounds;
                                        self->finish();
                                    });
                            }
                        }
                        continue;
                    }
                    if (u.cleanupPending) {
                        self->clean(u);
                        continue;
                    }
                    if (u.state == UploadState::Verifying && !u.cancelRequested && u.expiresAt > *now) {
                        self->advance(u, RequestId::parse(u.id.toString()).value());
                        continue;
                    }
                    // 过期检查及取消先以短事务确认，再清理；READY 不自动重试发布。
                    if (!u.cancelRequested && (u.expiresAt > *now || u.state == UploadState::Committing)) continue;
                    self->busy.emplace(u.id, true);
                    ++self->backgrounds;
                    self->use<Upload>(
                        [u](auto& service) { return service.read(u.id); },
                        [self, u](auto row) {
                            if (!row) {
                                self->release(u.id);
                                return;
                            }
                            auto expired = std::make_shared<bool>(false);
                            self->meta(
                                [row, expired](auto& r) -> Result<void> {
                                    *expired = row.value().expiresAt <= r.clock->now();
                                    return {};
                                },
                                [self, u, row, expired](auto checked) {
                                    if (!checked || (!row.value().cancelRequested &&
                                                     (!*expired || row.value().state == UploadState::Committing))) {
                                        self->release(u.id);
                                        return;
                                    }
                                    self->recoverOne(row.value(), [self, u](auto) { self->release(u.id); });
                                });
                        });
                }
            self->sweeping = false;
            --self->backgrounds;
            self->finish();
        });
}
struct UploadExecutor::Impl::Read : std::enable_shared_from_this<Read> {
    std::shared_ptr<Impl> owner;
    DownloadGrant grant;
    std::uint64_t id;
    bool slot, inFlight = true, cancelled = false, retired = false, closing = false;
    std::size_t pendingBytes = 0;
    OwnedTransferFile file;
    std::unique_ptr<BlobAccessRegistry::Lease> lease;
    std::function<void()> onCancelled;
    std::chrono::steady_clock::time_point progress = std::chrono::steady_clock::now();
    Read(std::shared_ptr<Impl> o, DownloadGrant g, std::uint64_t n, bool stream)
        : owner(std::move(o)), grant(std::move(g)), id(n), slot(stream) {}
    void close(std::function<void()> cb = {}) {
        if (closing || retired || inFlight) return;
        closing = true;
        auto self = shared_from_this();
        owner->io(
            [self](auto&) -> Result<void> {
                self->file.reset();
                return {};
            },
            [self, cb](auto result) {
                // 清理队列在协调器停止前仍可用；失败不能在loop销毁worker句柄。
                if (!result) std::terminate();
                self->retired = true;
                self->lease.reset();
                if (self->slot) --self->owner->readSlots;
                self->owner->reads.erase(self->id);
                if (cb) cb();
                self->owner->finish();
            },
            true);
    }
    void cancel() {
        auto self = shared_from_this();
        const bool first = !cancelled;
        cancelled = true;
        if (!inFlight) close();
        if (first) {
            auto notify = std::move(onCancelled);
            try { if (notify) notify(); } catch (...) {}
        }
    }
};
std::size_t UploadExecutor::Impl::userReads(UserId user) const {
    std::size_t count = 0;
    for (const auto& entry : reads)
        if (entry.second->slot && entry.second->grant.user == user) ++count;
    return count;
}
void UploadExecutor::Impl::disableReads(UserId user, bool confirmed) {
    // 已退休读取无需新metadata任务，尤其不能在停止屏障完成后重新生产工作。
    if (reads.empty()) return;
    auto affected = std::make_shared<std::vector<std::shared_ptr<Read>>>();
    for (const auto& entry : reads)
        if (entry.second->grant.user == user) affected->push_back(entry.second);
    if (confirmed) {
        for (auto& read : *affected) read->cancel();
        return;
    }
    // 未知提交不当作禁用成功；新快照复核。只处理通知时已有的读取资格。
    auto disabled = std::make_shared<bool>(false);
    auto self = shared_from_this();
    ++backgrounds;
    meta(
        [user, disabled](auto& r) -> Result<void> {
            auto opened = r.metadata->beginRead();
            if (!opened) return opened.error();
            auto found = opened.value()->users().find(user);
            if (!found) return found.error();
            *disabled = !found.value() || !found.value()->enabled;
            return {};
        },
        [self, affected, disabled](auto result) {
            if (result && *disabled)
                for (auto& read : *affected) read->cancel();
            --self->backgrounds;
            self->finish();
        });
}
namespace {
template <class State>
class ReadStream final : public DownloadStream {
    std::shared_ptr<State> state_;

public:
    explicit ReadStream(std::shared_ptr<State> s) : state_(std::move(s)) {}
    ~ReadStream() override { state_->cancel(); }
    void cancel() noexcept override { state_->cancel(); }
    void setCancellationCallback(std::function<void()> cb) override {
        state_->owner->loop.assertInLoopThread();
        if (state_->cancelled) { if (cb) cb(); return; }
        state_->onCancelled = std::move(cb);
    }
    void consumed(std::size_t bytes) noexcept override {
        auto& s = *state_;
        s.owner->loop.assertInLoopThread();
        if (bytes > s.pendingBytes) {
            s.cancel();
            return;
        }
        s.pendingBytes -= bytes;
        if (bytes) s.progress = std::chrono::steady_clock::now();
    }
    TaskSubmission pull(UploadOffset offset, std::size_t count, OwnedTransferBlock& block,
                        Completion<OwnedTransferBlock> cb) override {
        auto s = state_;
        s->owner->loop.assertInLoopThread();
        if (s->cancelled || s->retired || s->closing) return SubmitError::Stopping;
        if (s->inFlight || s->pendingBytes) return SubmitError::QueueFull;
        const auto length = s->grant.descriptor.blob.size.value();
        if (!cb || !block || !count || count > kTransferBlockBytes || offset.value() > length ||
            count > static_cast<std::uint64_t>(length - offset.value()))
            return SubmitError::InternalError;
        auto held = std::make_shared<OwnedTransferBlock>();
        auto submitted = s->owner->template command<OwnedTransferBlock>(
            std::move(cb),
            [s, held, offset, count](auto done) {
                if (s->cancelled) {
                    s->inFlight = false;
                    s->close();
                    done(ErrorCode::Retryable);
                    return;
                }
                s->owner->io(
                    [s, held, offset, count](auto& fs) { return status(fs.read(*s->file, offset, count, **held)); },
                    [s, held, count, done](auto result) mutable {
                        s->inFlight = false;
                        if (!result || s->cancelled) {
                            s->cancel();
                            done(result ? Error{ ErrorCode::Retryable } : result.error());
                            return;
                        }
                        if ((*held)->size != count) {
                            s->cancel();
                            done(ErrorCode::IntegrityError);
                            return;
                        }
                        s->pendingBytes = count;
                        s->progress = std::chrono::steady_clock::now();
                        done(std::move(*held));
                    });
            },
            [s] {
                s->inFlight = false;
                s->cancel();
            });
        if (std::holds_alternative<AcceptedTask>(submitted)) {
            s->inFlight = true;
            *held = std::move(block);
        }
        return submitted;
    }
};
}  // namespace
bool UploadExecutor::blobPinned(BlobId id) const {
    impl_->loop.assertInLoopThread();
    return impl_->access.pinned(id);
}
BlobAccessRegistry& UploadExecutor::blobAccess() { impl_->loop.assertInLoopThread(); return impl_->access; }
TaskSubmission UploadExecutor::submit(RequestContext context, OpenDownloadCommand command,
                                      Completion<OpenedDownload> cb) {
    auto s = impl_;
    return s->command<OpenedDownload>(std::move(cb), [s, context, command](auto done) {
        auto grant = std::make_shared<Result<DownloadGrant>>(ErrorCode::InternalError);
        s->meta(
            [grant, context, command](auto& r) -> Result<void> {
                auto* store = dynamic_cast<TransferMetadataStore*>(r.metadata.get());
                if (!store) return ErrorCode::InternalError;
                *grant = prepareDownload(*store, *r.clock, context, command.id);
                return {};
            },
            [s, grant, context, command, done](auto result) {
                if (!result) {
                    done(result.error());
                    return;
                }
                if (!*grant) {
                    done(grant->error());
                    return;
                }
                if (s->stopping || s->nextRead == UINT64_MAX) {
                    done(ErrorCode::Retryable);
                    return;
                }
                std::size_t user = s->userReads(grant->value().user);
                for (const auto& p : s->patches)
                    if (p.second->patch.upload.userId == grant->value().user) ++user;
                if (!command.metadataOnly && (s->patches.size() + s->readSlots >= 4 || user >= 2)) {
                    done(ErrorCode::Retryable);
                    return;
                }
                auto lease = s->access.tryAcquireRead(grant->value().descriptor.blob.id);
                if (!lease) { done(ErrorCode::Retryable); return; }
                auto read = std::make_shared<Impl::Read>(s, grant->value(), s->nextRead++, !command.metadataOnly);
                read->lease = std::move(lease);
                s->reads.emplace(read->id, read);  // pin在磁盘open前取得，close退休后释放。
                if (read->slot) ++s->readSlots;
                s->meta([read, context, command](auto& r) -> Result<void> {
                    auto* store = dynamic_cast<TransferMetadataStore*>(r.metadata.get());
                    if (!store) return ErrorCode::InternalError;
                    auto current = prepareDownload(*store, *r.clock, context, command.id);
                    if (!current) return current.error();
                    if (current.value().descriptor.blob.id != read->grant.descriptor.blob.id) return ErrorCode::Conflict;
                    return {};
                }, [s, read, command, done](auto checked) {
                    if (!checked || read->cancelled || s->stopping) {
                        read->inFlight = false;
                        read->close([done, checked] { done(checked ? Error{ErrorCode::Retryable} : checked.error()); });
                        return;
                    }
                    s->io(
                    [read](auto& fs) -> Result<void> {
                        auto opened = unwrap(fs.open(ObjectFile{ read->grant.descriptor.blob.id }, false));
                        if (!opened)
                            return opened.error().code == ErrorCode::NotFound ? Error{ ErrorCode::IntegrityError }
                                                                              : opened.error();
                        read->file = std::move(opened).value();
                        auto stat = unwrap(fs.stat(*read->file));
                        if (!stat) return stat.error();
                        if (stat.value().size.value() != read->grant.descriptor.blob.size.value())
                            return ErrorCode::IntegrityError;
                        return {};
                    },
                    [read, command, done](auto opened) {
                        read->inFlight = false;
                        if (!opened || read->cancelled || read->owner->stopping) {
                            read->cancelled = true;
                            read->close(
                                [done, opened] { done(opened ? Error{ ErrorCode::Retryable } : opened.error()); });
                            return;
                        }
                        if (command.metadataOnly) {
                            read->close([read, done] { done(OpenedDownload{ read->grant.descriptor, {} }); });
                            return;
                        }
                        auto weak = std::weak_ptr<Impl::Read>(read);
                        watchProgress<Impl::Read>(read->owner->loop, weak, [weak] {
                            if (auto r = weak.lock()) r->cancel();
                        });
                        done(OpenedDownload{ read->grant.descriptor, std::make_unique<ReadStream<Impl::Read>>(read) });
                    });
                });
            });
    });
}
void UploadExecutor::recover(std::function<void(Result<void>)> cb) {
    impl_->loop.assertInLoopThread();
    impl_->recover(std::move(cb));
}
void UploadExecutor::notifyUserDisabled(UserId user, bool confirmed) {
    impl_->loop.assertInLoopThread();
    impl_->disableReads(user, confirmed);
    impl_->sweep();
}
void UploadExecutor::Impl::stop(std::function<void()> cb) {
    stopping = true;
    onStopped = std::move(cb);
    auto activeReads = reads;
    for (auto& entry : activeReads) entry.second->cancel();
    auto active = patches;
    for (auto& entry : active) entry.second->cancel();
    finish();
}
UploadExecutor::UploadExecutor(muduox::net::EventLoop& l, MetadataExecutor& m, DiskExecutor& d, DiskSafetyPolicy s)
    : impl_(std::make_shared<Impl>(l, m, d, s)) {}
UploadExecutor::~UploadExecutor() {
    if (impl_->commands || impl_->backgrounds || !impl_->patches.empty() || !impl_->reads.empty()) std::terminate();
}
void UploadExecutor::requestStop(std::function<void()> cb) {
    impl_->loop.assertInLoopThread();
    impl_->stop(std::move(cb));
}
bool UploadExecutor::stopped() const { return impl_->finished; }
TaskSubmission UploadExecutor::submit(RequestContext ctx, CreateUploadCommand cmd, Completion<UploadCreated> cb) {
    auto s = impl_;
    return s->command<UploadCreated>(std::move(cb), [s, ctx, cmd](auto done) {
        s->use<std::optional<Upload>>(
            [ctx, cmd](auto& service) { return service.findReplay(ctx, cmd); },
            [s, ctx, cmd, done](auto replay) {
                if (!replay) {
                    done(replay.error());
                    return;
                }
                if (replay.value()) {
                    const auto row = *replay.value();
                    s->clean(row);
                    s->use<UploadView>(
                        [row](auto& service) { return service.view(row); },
                        [done, row](auto view) {
                            if (!view)
                                done(view.error());
                            else
                                done(UploadCreated{ view.value(), row.state == UploadState::Creating
                                                                      ? UploadCreateDisposition::Creating
                                                                      : UploadCreateDisposition::Existing });
                        });
                    return;
                }
                auto capacity = std::make_shared<Result<DiskCapacity>>(ErrorCode::InternalError);
                s->io(
                    [capacity](auto& fs) -> Result<void> {
                        *capacity = unwrap(fs.capacity());
                        return {};
                    },
                    [s, ctx, cmd, done, capacity](auto sampled) {
                        if (!sampled || !*capacity) {
                            done(!sampled ? sampled.error() : capacity->error());
                            return;
                        }
                        auto margin = diskSafetyMargin(capacity->value().volume, s->safety);
                        if (!margin) {
                            done(margin.error());
                            return;
                        }
                        s->use<UploadCreation>(
                            [ctx, cmd, capacity, margin](auto& service) {
                                return service.create(ctx, cmd, capacity->value().free, margin.value());
                            },
                            [s, done](auto created) {
                                if (!created) {
                                    done(created.error());
                                    return;
                                }
                                auto u = created.value().upload;
                                auto respond = [s, done](Upload row, UploadCreateDisposition disposition) {
                                    s->use<UploadView>([row](auto& service) { return service.view(row); },
                                                       [done, disposition](auto view) {
                                                           if (!view)
                                                               done(view.error());
                                                           else
                                                               done(UploadCreated{ view.value(), disposition });
                                                       });
                                };
                                if (!created.value().inserted) {
                                    respond(u, u.state == UploadState::Creating ? UploadCreateDisposition::Creating
                                                                                : UploadCreateDisposition::Existing);
                                    return;
                                }
                                s->busy.emplace(u.id, true);
                                ++s->backgrounds;
                                s->io(
                                    [u](auto& fs) -> Result<void> {
                                        auto file = unwrap(fs.create(u.id));
                                        if (!file) return file.error();
                                        return {};
                                    },
                                    [s, u, respond, done](auto file) {
                                        if (!file) {
                                            if (file.error().code == ErrorCode::StorageFull ||
                                                file.error().code == ErrorCode::Retryable) {
                                                s->use<Upload>(
                                                    [u, file](auto& service) {
                                                        return service.terminate(
                                                            u.id, u.generation, UploadState::Failed,
                                                            file.error().code == ErrorCode::StorageFull
                                                                ? TransferFailure::StorageFull
                                                                : TransferFailure::TemporarilyUnavailable,
                                                            RequestId::parse(u.id.toString()).value());
                                                    },
                                                    [s, u, file, done](auto terminal) {
                                                        s->release(u.id);
                                                        if (terminal) s->clean(terminal.value());
                                                        done(file.error());
                                                    });
                                            } else {
                                                s->frozen.insert(u.id);
                                                s->release(u.id);
                                                done(file.error());
                                            }
                                            return;
                                        }
                                        s->use<Upload>(
                                            [u](auto& service) { return service.opened(u.id, u.generation); },
                                            [s, u, respond, done](auto opened) {
                                                s->release(u.id);
                                                if (!opened)
                                                    done(opened.error());
                                                else
                                                    respond(opened.value(), UploadCreateDisposition::Created);
                                            });
                                    });
                            });
                    });
            });
    });
}
TaskSubmission UploadExecutor::submit(RequestContext ctx, ListUploadsCommand cmd,
                                      Completion<Page<UploadView, UploadCursor>> cb) {
    auto s = impl_;
    return s->command<Page<UploadView, UploadCursor>>(std::move(cb), [s, ctx, cmd](auto done) {
        s->use<Page<UploadView, UploadCursor>>([ctx, cmd](auto& service) { return service.list(ctx, cmd); }, done);
    });
}
TaskSubmission UploadExecutor::submit(RequestContext ctx, GetUploadCommand cmd, Completion<UploadView> cb) {
    auto s = impl_;
    return s->command<UploadView>(std::move(cb), [s, ctx, cmd](auto done) {
        s->use<UploadView>([ctx, cmd](auto& service) { return service.get(ctx, cmd.id); },
                           [s, done](auto value) {
                               const bool terminal = value && isTerminal(value.value().state);
                               done(std::move(value));
                               if (terminal) s->sweep();
                           });
    });
}
TaskSubmission UploadExecutor::submit(RequestContext ctx, CancelUploadCommand cmd, Completion<void> cb) {
    auto s = impl_;
    return s->command<void>(std::move(cb), [s, ctx, cmd](auto done) {
        s->use<Upload>([ctx, cmd](auto& service) { return service.cancel(ctx, cmd.id); },
                       [s, done](auto cancelled) {
                           if (!cancelled) {
                               done(cancelled.error());
                               return;
                           }
                           auto patch = s->patches.find(cancelled.value().id);
                           if (patch != s->patches.end()) patch->second->cancel();
                           s->clean(cancelled.value());
                           done({});
                       });
    });
}
TaskSubmission UploadExecutor::submit(RequestContext ctx, CompleteUploadCommand cmd,
                                      Completion<UploadCompletionAccepted> cb) {
    auto s = impl_;
    return s->command<UploadCompletionAccepted>(std::move(cb), [s, ctx, cmd](auto done) {
        // 授权先于 busy 响应，避免暴露其他用户的上传。
        s->use<UploadView>(
            [ctx, cmd](auto& service) { return service.get(ctx, cmd.id); },
            [s, ctx, cmd, done](auto visible) {
                if (!visible) {
                    done(visible.error());
                    return;
                }
                const bool exclusive =
                    visible.value().state == UploadState::Open || visible.value().state == UploadState::Ready;
                if (s->frozen.count(cmd.id)) {
                    done(ErrorCode::Retryable);
                    return;
                }
                if (s->patches.count(cmd.id) || (exclusive && s->busy.count(cmd.id))) {
                    done(ErrorCode::UploadBusy);
                    return;
                }
                if (exclusive) {
                    s->busy.emplace(cmd.id, true);
                    ++s->backgrounds;
                }
                s->use<Upload>([ctx, cmd](auto& service) { return service.acceptComplete(ctx, cmd); },
                               [s, ctx, cmd, done, exclusive](auto accepted) {
                                   if (exclusive) s->release(cmd.id);
                                   if (!accepted) {
                                       done(accepted.error());
                                       return;
                                   }
                                   auto row = accepted.value();
                                   s->advance(row, ctx.requestId);
                                   s->use<UploadView>(
                                       [row](auto& service) { return service.view(row); },
                                       [done, row](auto view) {
                                           if (!view)
                                               done(view.error());
                                           else
                                               done(UploadCompletionAccepted{
                                                   view.value(), row.state == UploadState::Completed
                                                                     ? UploadCompleteDisposition::Completed
                                                                     : UploadCompleteDisposition::Accepted });
                                       });
                               });
            });
    });
}
TaskSubmission UploadExecutor::submit(RequestContext ctx, PreparePatchCommand cmd, Completion<PatchPreparation> cb) {
    auto s = impl_;
    return s->command<PatchPreparation>(std::move(cb), [s, ctx, cmd](auto done) {
        s->use<UploadPatch>(
            [ctx, cmd](auto& service) { return service.preparePatch(ctx, cmd); },
            [s, ctx, cmd, done](auto prepared) {
                if (!prepared) {
                    if (prepared.error().code == ErrorCode::UploadOffsetMismatch)
                        s->use<UploadView>([ctx, cmd](auto& service) { return service.get(ctx, cmd.id); },
                                           [done](auto view) {
                                               if (!view)
                                                   done(view.error());
                                               else
                                                   done(PatchPreparation{ PatchOffsetConflict{ view.value().offset } });
                                           });
                    else
                        done(prepared.error());
                    return;
                }
                if (s->stopping) {
                    done(ErrorCode::Retryable);
                    return;
                }
                if (s->frozen.count(cmd.id)) {
                    done(ErrorCode::Retryable);
                    return;
                }
                if (s->busy.count(cmd.id)) {
                    done(ErrorCode::UploadBusy);
                    return;
                }
                std::size_t user = s->userReads(prepared.value().upload.userId);
                for (const auto& p : s->patches)
                    if (p.second->patch.upload.userId == prepared.value().upload.userId) ++user;
                if (s->patches.size() + s->readSlots >= 4 || user >= 2) {
                    done(ErrorCode::Retryable);
                    return;
                }
                auto patch = std::make_shared<Impl::Patch>(s, ctx, prepared.value());
                s->patches.emplace(cmd.id, patch);
                s->busy.emplace(cmd.id, true);
                patch->inFlight = true;
                s->io(
                    [patch](auto& fs) -> Result<void> {
                        auto opened = unwrap(fs.open(TemporaryFile{ patch->patch.upload.id }, true));
                        if (!opened) return opened.error();
                        patch->file = std::move(opened).value();
                        auto size = unwrap(fs.stat(*patch->file));
                        if (!size) return size.error();
                        if (size.value().size.value() != patch->written.value()) return ErrorCode::IntegrityError;
                        return {};
                    },
                    [patch, done](auto opened) {
                        patch->inFlight = false;
                        if (!opened || patch->cancelled) {
                            patch->owner->io(
                                [patch](auto&) -> Result<void> {
                                    patch->file.reset();
                                    return {};
                                },
                                [patch, done, opened](auto) {
                                    patch->retire();
                                    done(opened ? Error{ ErrorCode::Retryable } : opened.error());
                                },
                                true);
                            return;
                        }
                        patch->watch();
                        done(PatchPreparation{ PreparedPatch{ patch->written, patch->patch.upload.generation,
                                                              std::make_unique<Stream<Impl::Patch>>(patch) } });
                    });
            });
    });
}
TaskSubmission UploadExecutor::submit(RequestContext ctx, GetStorageCommand, Completion<StorageView> cb) {
    auto s = impl_;
    return s->command<StorageView>(std::move(cb), [s, ctx](auto done) {
        s->use<StorageView>([ctx](auto& service) { return service.storage(ctx); },
                            [s, done](auto value) { s->sampleStorage(std::move(value), done); });
    });
}
TaskSubmission UploadExecutor::submit(RequestContext ctx, SetStorageBudgetCommand cmd, Completion<StorageView> cb) {
    auto s = impl_;
    return s->command<StorageView>(std::move(cb), [s, ctx, cmd](auto done) {
        s->use<StorageView>([ctx, cmd](auto& service) { return service.setBudget(ctx, cmd); },
                            [s, done](auto value) { s->sampleStorage(std::move(value), done); });
    });
}
}  // namespace anof::runtime
