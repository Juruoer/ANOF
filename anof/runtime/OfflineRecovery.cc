#include <anof/application/drive/MaintenanceService.h>
#include <anof/application/maintenance/internal/FsckSupport.h>
#include <anof/application/transfer/UploadService.h>
#include <anof/infrastructure/storage/local/LocalMaintenanceFileStore.h>
#include <anof/infrastructure/storage/local/LocalTransferFileStore.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/runtime/OfflineRecovery.h>
namespace anof::runtime {
namespace {
using namespace maintenance;
template <class T>
T value(const MaintenanceContext& c, Result<T> r) {
    if (!r) {
        auto e = failure(c, r.error().code == ErrorCode::CommitOutcomeUnknown ? MaintenanceError::CommitOutcomeUnknown : MaintenanceError::IntegrityError, MaintenanceStage::Recover);
        if (r.error().code == ErrorCode::CommitOutcomeUnknown) e.outcome = MaintenanceOutcome::Unknown;
        throw e;
    }
    return std::move(r).value();
}
void value(const MaintenanceContext& c, Result<void> r) {
    if (!r) {
        auto e = failure(c, r.error().code == ErrorCode::CommitOutcomeUnknown ? MaintenanceError::CommitOutcomeUnknown : MaintenanceError::IntegrityError, MaintenanceStage::Recover);
        if (r.error().code == ErrorCode::CommitOutcomeUnknown) e.outcome = MaintenanceOutcome::Unknown;
        throw e;
    }
}
template <class T>
T file(const MaintenanceContext& c, FileResult<T> r) {
    if (auto* e = std::get_if<TransferFileError>(&r)) {
        auto code = *e == TransferFileError::UnknownEffect ? MaintenanceError::PublicationOutcomeUnknown : *e == TransferFileError::NoSpace ? MaintenanceError::StorageFull
                                                                                                                                            : MaintenanceError::IntegrityError;
        auto out = failure(c, code, MaintenanceStage::Recover);
        if (*e == TransferFileError::UnknownEffect) out.outcome = MaintenanceOutcome::Unknown;
        throw out;
    }
    return std::move(std::get<T>(r));
}
class Recovery final : public MaintenanceRecovery {
    MaintenanceResult<void> resume(const MaintenanceContext& c, const InstanceLease& lease, const ResumeUpload& p) override {
        bool effects = false;
        try {
            check(c, MaintenanceStage::Recover);
            auto r = value(c, makeInstanceMetadataResources(lease.root() / ".anof/metadata.sqlite3"));
            auto* store = dynamic_cast<TransferMetadataStore*>(r.metadata.get());
            if (!store) throw failure(c, MaintenanceError::InternalError);
            auto fs = file(c, infrastructure::storage::openLocalTransferFileStore(lease.root()));
            auto inspect = value(c, infrastructure::storage::makeLocalMaintenanceFileStore());
            UploadService service(*store, *r.clock, *r.transferIds, *r.nodeIds);
            auto u = value(c, service.read(p.id));
            if (u.generation.value() < p.generation.value()) throw failure(c, MaintenanceError::StalePlan);
            auto request = RequestId::parse(u.id.toString()).value();
            auto observe = [&](MaintenanceFileKey key, InspectionMode mode) { return take(inspect->observe(c, lease, key, mode)); };
            auto verified = [&](MaintenanceFileKey key) {auto f=observe(key,InspectionMode::Full);if(!f||f->evidence.size.value()!=u.original.length.value()||!f->sha256||(u.actualSha256&&f->sha256!=u.actualSha256))throw failure(c,MaintenanceError::IntegrityError);return *f->sha256; };
            // 与在线恢复共用UploadService的generation/预留/发布事务，不开周期扫描或清理其他上传。
            if (u.state == UploadState::Committing) {
                auto temp = observe(u.id, InspectionMode::Quick), object = observe(*u.pendingBlobId, InspectionMode::Quick);
                if (bool(temp) == bool(object)) throw failure(c, MaintenanceError::IntegrityError);
                if (u.cancelRequested && !object) {
                    effects = true;
                    u = value(c, service.terminate(u.id, u.generation, UploadState::Cancelled, TransferFailure::UserDisabled, request));
                } else {
                    verified(object ? MaintenanceFileKey{ *u.pendingBlobId } : MaintenanceFileKey{ u.id });
                    check(c, MaintenanceStage::Recover);
                    effects = true;
                    if (object)
                        file(c, fs->syncPublication(u.id, *u.pendingBlobId));
                    else
                        file(c, fs->publish(u.id, *u.pendingBlobId));
                    u = value(c, service.markObject(u.id, u.generation));
                    if (u.cancelRequested)
                        u = value(c, service.terminate(u.id, u.generation, UploadState::Cancelled, TransferFailure::UserDisabled, request));
                    else {
                        auto committed = service.commitPublish(u.id, u.generation, request);
                        if (!committed) {
                            const auto code = committed.error().code;
                            if (code != ErrorCode::NameConflict && code != ErrorCode::NotFound && code != ErrorCode::ParentNotDirectory && code != ErrorCode::DepthLimitExceeded) value(c, std::move(committed));
                            const auto reason = code == ErrorCode::NameConflict ? TransferFailure::NameConflict : code == ErrorCode::NotFound         ? TransferFailure::NotFound
                                                                                                              : code == ErrorCode::ParentNotDirectory ? TransferFailure::ParentNotDirectory
                                                                                                                                                      : TransferFailure::DepthLimitExceeded;
                            u = value(c, service.publicationConflict(u.id, u.generation, reason));
                        } else
                            u = std::move(committed).value();
                    }
                }
            } else if (static_cast<int>(u.state) < 5 && (u.cancelRequested || u.expiresAt <= r.clock->now())) {
                effects = true;
                u = value(c, service.terminate(u.id, u.generation, u.cancelRequested ? UploadState::Cancelled : UploadState::Expired, u.cancelRequested ? TransferFailure::UserDisabled : TransferFailure::UploadExpired, request));
            }
            if (u.cleanupPending) {
                check(c, MaintenanceStage::Recover);
                effects = true;
                file(c, fs->removeAndSync(u.location == StagingLocation::Object ? TransferFileKey{ ObjectFile{ *u.pendingBlobId } } : TransferFileKey{ TemporaryFile{ u.id } }));
                value(c, service.confirmCleanup(u.id, u.generation));
                return {};
            }
            if (static_cast<int>(u.state) >= 5) return {};
            if (u.state == UploadState::Ready) {
                verified(u.location == StagingLocation::Object ? MaintenanceFileKey{ *u.pendingBlobId } : MaintenanceFileKey{ u.id });
                return {};
            }
            if (u.state == UploadState::Verifying) {
                const auto sha = verified(u.id);
                check(c, MaintenanceStage::Recover);
                effects = true;
                u = value(c, service.recordVerification(u.id, u.generation, sha, request));
                if (u.cleanupPending) {
                    file(c, fs->removeAndSync(TemporaryFile{ u.id }));
                    value(c, service.confirmCleanup(u.id, u.generation));
                }
                return {};
            }
            auto observed = observe(u.id, InspectionMode::Quick);
            if (!observed) {
                if (u.state != UploadState::Creating) throw failure(c, MaintenanceError::IntegrityError);
                effects = true;
                auto created = file(c, fs->create(u.id));
            } else if (observed->evidence.size.value() < u.durableOffset.value() || (u.state == UploadState::Creating && observed->evidence.size.value() != 0))
                throw failure(c, MaintenanceError::IntegrityError);
            auto opened = file(c, fs->open(TemporaryFile{ u.id }, true));
            check(c, MaintenanceStage::Recover);
            effects = true;
            file(c, fs->truncateAndSync(*opened, u.durableOffset));
            opened.reset();
            if (u.state == UploadState::Creating) value(c, service.opened(u.id, u.generation));
            return {};
        } catch (MaintenanceFailure e) {
            if (effects && e.outcome == MaintenanceOutcome::NotStarted) e.outcome = MaintenanceOutcome::Confirmed;
            return e;
        }
    }
    MaintenanceResult<void> remove(const MaintenanceContext& c, const InstanceLease& lease, const ResumeDeletingBlob& p) override {
        try {
            check(c, MaintenanceStage::Recover);
            auto r = value(c, makeInstanceMetadataResources(lease.root() / ".anof/metadata.sqlite3"));
            auto* store = dynamic_cast<DriveMetadataStore*>(r.metadata.get());
            if (!store) throw failure(c, MaintenanceError::InternalError);
            {
                auto read = value(c, store->beginDriveRead());
                auto b = value(c, read->blobLifecycle().record(p.id));
                if (!b || b->state != BlobState::Deleting || b->revision != p.revision || value(c, read->blobs().hasNodeReference(p.id)) || value(c, read->blobs().hasUploadReference(p.id))) throw failure(c, MaintenanceError::StalePlan);
            }
            auto fs = file(c, infrastructure::storage::openLocalTransferFileStore(lease.root()));
            file(c, fs->removeAndSync(ObjectFile{ p.id }));
            return {};
        } catch (const MaintenanceFailure& e) {
            return e;
        }
    }
    MaintenanceResult<void> finalize(const MaintenanceContext& c, const InstanceLease& lease, const ResumeDeletingBlob& p) override {
        try {
            check(c, MaintenanceStage::Recover);
            auto r = value(c, makeInstanceMetadataResources(lease.root() / ".anof/metadata.sqlite3"));
            auto* store = dynamic_cast<DriveMetadataStore*>(r.metadata.get());
            if (!store) throw failure(c, MaintenanceError::InternalError);
            {
                auto read = value(c, store->beginDriveRead());
                auto b = value(c, read->blobLifecycle().record(p.id));
                if (!b) return {};
                if (b->state != BlobState::Deleting || b->revision != p.revision) throw failure(c, MaintenanceError::StalePlan);
            }
            auto files = value(c, infrastructure::storage::makeLocalMaintenanceFileStore());
            if (take(files->observe(c, lease, p.id, InspectionMode::Quick))) throw failure(c, MaintenanceError::IntegrityError);
            MaintenanceService service(*store, *r.clock);
            value(c, service.finalize(p.id, p.revision, RequestId::parse(c.operationId.toString()).value()));
            return {};
        } catch (const MaintenanceFailure& e) {
            return e;
        }
    }
};
}  // namespace
std::unique_ptr<MaintenanceRecovery> makeOfflineMaintenanceRecovery() { return std::make_unique<Recovery>(); }
}  // namespace anof::runtime
