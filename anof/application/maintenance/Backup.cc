#include <anof/application/maintenance/Backup.h>
#include <anof/application/maintenance/PrepareInstance.h>
#include <anof/application/maintenance/internal/FsckSupport.h>
namespace anof {
using namespace maintenance;
BackupService::BackupService(OfflineInstanceAccess& a, MetadataInspection& i, MetadataSnapshot& s, MaintenanceFileStore& f,
                             InstanceConfigurationLoader& config, MaintenanceRecovery& r, MaintenanceIdGenerator& ids, Clock& clock, FsckService& fsck)
    : access_(a), inspection_(i), snapshot_(s), files_(f), configuration_(config), recovery_(r), ids_(ids), clock_(clock), fsck_(fsck) {}
MaintenanceResult<BackupResult> BackupService::backup(const MaintenanceContext& c, const BackupRequest& request) {
    bool effects = false;
    std::optional<std::filesystem::path> artifact;
    try {
        check(c, MaintenanceStage::Validate);
        std::unique_ptr<InstanceLease> lease;
        std::unique_ptr<MaintenanceDestination> destination;
        if (request.output.lexically_normal() < request.root.lexically_normal()) {
            destination = take(access_.acquireDestination(c, request.root, request.output));
            lease = take(access_.acquireExisting(c, request.root));
        } else {
            lease = take(access_.acquireExisting(c, request.root));
            destination = take(access_.acquireDestination(c, request.root, request.output));
        }
        artifact = destination->staging();
        auto loadedConfig = configuration_.load(*lease);
        if (!loadedConfig) throw failure(c, loadedConfig.error().code == ErrorCode::InvalidArgument ? MaintenanceError::InvalidArgument : loadedConfig.error().code == ErrorCode::Forbidden || loadedConfig.error().code == ErrorCode::NotFound ? MaintenanceError::UnsafePath
                                                                                                                                                                                                                                                : MaintenanceError::IoError,
                                         MaintenanceStage::Validate);
        auto config = std::move(loadedConfig).value();
        {
            auto captured = take(access_.captureInspection(c, *lease, request.workDirectory));
            auto reader = take(inspection_.open(c, *captured));
            pages<MaintenanceRunId>(c, [&](auto after) { return reader->runs(c, PageSize::from(200).value(), after); }, [&](const MaintenanceRun& run) {if(!run.completed)throw failure(c,MaintenanceError::MaintenancePending); });
        }
        auto inspected = take(fsck_.inspectHeld(c, *lease, { request.root, request.workDirectory, InspectionMode::Full, true }, {}));
        if (!inspected.plan) throw failure(c, MaintenanceError::CheckIncomplete);
        // 备份只推进已有责任；不以备份为由标记损坏、修账或隔离。
        for (const auto& a : inspected.plan->actions)
            if (!std::holds_alternative<ResumeUpload>(a.payload) && !std::holds_alternative<ResumeDeletingBlob>(a.payload) && !std::holds_alternative<TruncateUnconfirmedTail>(a.payload)) throw failure(c, MaintenanceError::IntegrityError);
        for (const auto& a : inspected.plan->actions) {
            check(c, MaintenanceStage::Recover);
            effects = true;
            if (auto p = std::get_if<ResumeUpload>(&a.payload))
                take(recovery_.resume(c, *lease, *p));
            else if (auto deleting = std::get_if<ResumeDeletingBlob>(&a.payload)) {
                take(recovery_.remove(c, *lease, *deleting));
                take(recovery_.finalize(c, *lease, *deleting));
            } else
                take(files_.truncateTail(c, *lease, std::get<TruncateUnconfirmedTail>(a.payload)));
        }
        bool quarantine = false, other = false;
        auto verified = take(fsck_.inspectHeld(c, *lease, { request.root, request.workDirectory, InspectionMode::Full, false }, [&](const MaintenanceFinding& f) -> MaintenanceResult<void> {
            if (f.code == FindingCode::QuarantinePresent)
                quarantine = true;
            else if (f.severity != FindingSeverity::Info)
                other = true;
            return {};
        }));
        if (!verified.report.complete || other) throw failure(c, MaintenanceError::IntegrityError);
        if (quarantine && !request.excludeQuarantine) throw failure(c, MaintenanceError::QuarantinePresent);
        if (!verified.identity.instance) throw failure(c, MaintenanceError::IntegrityError);
        // 已有UNAVAILABLE即使未引用也不可通过归一化丢弃。
        auto capture = take(access_.captureInspection(c, *lease, request.workDirectory));
        auto reader = take(inspection_.open(c, *capture));
        pages<BlobId>(c, [&](auto after) { return reader->objects(c, PageSize::from(200).value(), after); }, [&](const InspectedObject& o) {if(o.record.state==BlobState::Unavailable)throw failure(c,MaintenanceError::IntegrityError); });
        take(snapshot_.exportConsistent(c, *lease, *destination));
        auto exclusions = take(snapshot_.normalizeBackup(c, *destination));
        pages<BlobId>(c, [&](auto after) { return reader->objects(c, PageSize::from(200).value(), after); }, [&](const InspectedObject& o) {
    if (!o.nodeReference) { return; }
            check(c,MaintenanceStage::Copy);
            const auto& blob=o.record.content;
    auto observed=take(files_.observe(c,*lease,blob.id,InspectionMode::Full));
    if(!observed||observed->sha256!=blob.sha256||observed->evidence.size.value()!=blob.size.value())throw failure(c,MaintenanceError::IntegrityError);
    take(files_.copyObject(c,*lease,*destination,blob,observed->evidence));take(files_.appendObjectIndex(c,*destination,{blob})); });
        reader.reset();
        capture.reset();
        auto index = take(files_.finishObjectIndex(c, *destination));
        auto configDigest = take(files_.writeConfiguration(c, *destination, config));
        auto database = take(snapshot_.verifyBackup(c, *destination));
        BackupManifest manifest{ 1, "sqlite", "0.1.0-dev", 5, 5, checked(c, ids_.nextBackup()), *verified.identity.instance, clock_.now(), database, configDigest, index, exclusions };
        take(files_.sealBackup(c, *destination, manifest));
        take(files_.publish(c, *destination));
        return BackupResult{ std::move(manifest), request.output };
    } catch (MaintenanceFailure e) {
        if (effects && e.outcome == MaintenanceOutcome::NotStarted) e.outcome = MaintenanceOutcome::Confirmed;
        if (!e.artifact) e.artifact = artifact;
        return e;
    } catch (...) {
        auto e = failure(c, MaintenanceError::InternalError);
        if (effects) e.outcome = MaintenanceOutcome::Unknown;
        e.artifact = artifact;
        return e;
    }
}
MaintenanceResult<RestoreResult> BackupService::restore(const MaintenanceContext& c, const RestoreRequest& request) {
    std::optional<std::filesystem::path> artifact;
    try {
        check(c, MaintenanceStage::Validate);
        if (!validateInstanceConfiguration(request.configuration)) throw failure(c, MaintenanceError::InvalidArgument, MaintenanceStage::Validate);
        auto destination = take(access_.acquireDestination(c, request.backup, request.root));
        artifact = destination->staging();
        auto source = take(files_.openBackup(c, request.backup));
        const auto instance = source->manifest().instance;
        take(files_.copyBackup(c, *source, *destination));
        take(files_.writeConfiguration(c, *destination, request.configuration));
        take(snapshot_.prepareRestore(c, *destination, instance));
        take(files_.sealRestoredInstance(c, *destination, instance));
        take(files_.publish(c, *destination));
        return RestoreResult{ instance, request.root };
    } catch (MaintenanceFailure e) {
        if (!e.artifact) e.artifact = artifact;
        return e;
    } catch (...) {
        auto e = failure(c, MaintenanceError::InternalError);
        e.artifact = artifact;
        e.outcome = MaintenanceOutcome::Unknown;
        return e;
    }
}
}  // namespace anof
