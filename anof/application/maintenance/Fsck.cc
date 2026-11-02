#include <anof/application/maintenance/internal/FsckSupport.h>

#include <algorithm>
namespace anof {
using namespace maintenance;
FsckService::FsckService(OfflineInstanceAccess& a, MetadataInspection& i, MaintenanceFileStore& f, MetadataRepair& r,
                         MaintenanceRecovery& recovery, MaintenanceDigest& digest, MaintenanceIdGenerator& ids, Clock& clock)
    : access_(a), inspection_(i), files_(f), repair_(r), recovery_(recovery), digest_(digest), ids_(ids), clock_(clock) {}
MaintenanceResult<FsckResult> FsckService::inspect(const MaintenanceContext& c, const FsckRequest& request, const FindingSink& sink) {
    try {
        check(c);
        if (request.mode != InspectionMode::Quick && request.mode != InspectionMode::Full) throw failure(c, MaintenanceError::InvalidArgument);
        auto lease = take(access_.acquireExisting(c, request.root));
        return inspectHeld(c, *lease, request, sink);
    } catch (const MaintenanceFailure& e) {
        return e;
    } catch (const std::exception&) {
        return failure(c, MaintenanceError::InternalError);
    }
}
MaintenanceResult<FsckResult> FsckService::inspectHeld(const MaintenanceContext& c, const InstanceLease& lease, const FsckRequest& request, const FindingSink& sink) {
    try {
        auto capture = take(access_.captureInspection(c, lease, request.workDirectory));
        auto reader = take(inspection_.open(c, *capture));
        auto identity = take(reader->identity(c));
        MaintenanceReport report;
        std::size_t details = 0, actionBytes = 0;
        bool safePlan = true, accountingMismatch = false, pending = false;
        std::vector<MaintenanceAction> actions;
        auto emit = [&](MaintenanceFinding f) {
            check(c);
            auto& n = f.severity == FindingSeverity::Error ? report.errors : f.severity == FindingSeverity::Warning ? report.warnings
                                                                                                                    : report.info;
            n = checked(c, n.add(1));
            if (details < kMaintenanceFindingLimit) {
                if (sink) take(sink(f));
                ++details;
            } else {
                report.detailsTruncated = true;
                report.complete = false;
            }
        };
        auto finding = [&](FindingCode code, const MaintenanceFileKey& key, bool repairable = false) {
            auto resource = std::holds_alternative<BlobId>(key) ? MaintenanceResource::Blob : std::holds_alternative<UploadId>(key) ? MaintenanceResource::Upload
                                                                                                                                    : MaintenanceResource::Quarantine;
            auto id = std::visit([](auto value) { return MaintenanceResourceId::parse(value.toString()).value(); }, key);
            emit({ code, FindingSeverity::Error, resource, id, key, repairable });
        };
        auto add = [&](MaintenanceActionPayload payload) {
            if (!request.plan) return;
            auto encoded = maintenanceActionBytes(payload);
            if (actions.size() >= kMaintenanceActionLimit || actionBytes + encoded.size() + 160 > kMaintenancePlanBytes - 1024) {
                report.complete = false;
                report.detailsTruncated = true;
                return;
            }
            actionBytes += encoded.size() + 160;
            actions.push_back({ checked(c, ids_.nextAction()), std::move(payload), checked(c, digest_.digest(encoded)) });
        };
        pages<InspectionPosition>(c, [&](auto after) { return reader->findings(c, PageSize::from(200).value(), after); }, [&](auto f) {
            accountingMismatch|=f.code==FindingCode::AccountingMismatch;
            pending|=f.code==FindingCode::MaintenancePending;
            if(f.code!=FindingCode::AccountingMismatch&&f.code!=FindingCode::QuarantinePresent&&f.code!=FindingCode::MaintenancePending)safePlan=false;
            f.repairable=f.code==FindingCode::AccountingMismatch;
            emit(f); });
        auto initial = take(reader->summary(c));
        report.info = initial.info;
        report.warnings = initial.warnings;
        report.errors = initial.errors;
        report.complete &= initial.complete;
        report.detailsTruncated |= initial.detailsTruncated;
        // 无法解释结构时仍返回已有诊断，不通过普通业务opener重新开源库。
        if (initial.complete && safePlan) {
            auto account = take(reader->accounting(c));
            if (accountingMismatch && account.complete && account.provenLedger)
                add(RecomputeAccounting{ *account.provenLedger });
            else if (accountingMismatch)
                safePlan = false;
            auto observe = [&](const MaintenanceFileKey& key, InspectionMode mode) { return take(files_.observe(c, lease, key, mode)); };
            auto object = [&](BlobId id) -> std::optional<InspectedObject> {
                auto page = take(reader->objects(c, PageSize::from(1).value(), before(id)));
                if (!page.items.empty() && page.items.front().record.content.id == id) return page.items.front();
                return {};
            };
            auto upload = [&](UploadId id) -> std::optional<Upload> {
                auto page = take(reader->uploads(c, PageSize::from(1).value(), before(id)));
                if (!page.items.empty() && page.items.front().id == id) return page.items.front();
                return {};
            };
            auto quarantine = [&](QuarantineId id) -> std::optional<QuarantineEntry> {
                auto page = take(reader->quarantine(c, PageSize::from(1).value(), before(id)));
                if (!page.items.empty() && page.items.front().id == id) return page.items.front();
                return {};
            };
            pages<BlobId>(c, [&](auto after) { return reader->objects(c, PageSize::from(200).value(), after); }, [&](const InspectedObject& row) {
                const auto& b=row.record;
                if(b.state==BlobState::Deleting) {
                    if(row.nodeReference||row.uploadReference) {finding(FindingCode::UploadStateInconsistent,b.content.id);safePlan=false;return;}
                    finding(FindingCode::MaintenancePending,b.content.id,true);
                    add(ResumeDeletingBlob{b.content.id,b.revision,checked(c,digest_.digest(b.content.id.toString()+":"+std::to_string(b.revision.value())+":"+std::to_string(b.chargedBytes.value())))});
                    return;
                }
                auto file=observe(b.content.id,request.mode);
                std::optional<UnavailableEvidence> damage;
                if(!file)damage=UnavailableEvidence{UnavailableReason::Missing,{},{}};
                else if(file->evidence.size.value()!=b.content.size.value())damage=UnavailableEvidence{UnavailableReason::Length,file->evidence,{}};
                else if(request.mode==InspectionMode::Full&&file->sha256!=b.content.sha256)damage=UnavailableEvidence{UnavailableReason::Hash,file->evidence,file->sha256};
                if(damage) {
                    finding(damage->reason==UnavailableReason::Missing?FindingCode::ObjectMissing:damage->reason==UnavailableReason::Length?FindingCode::ObjectLengthMismatch:FindingCode::ObjectHashMismatch,b.content.id,true);
                    auto payload=MarkBlobUnavailable{b.content.id,b.revision,*damage};
                    if(b.state!=BlobState::Unavailable||!b.unavailable||maintenanceActionBytes(payload)!=maintenanceActionBytes(MarkBlobUnavailable{b.content.id,b.revision,*b.unavailable}))add(payload);
                } else if(b.state==BlobState::Unavailable) {
                    finding(FindingCode::MaintenancePending,b.content.id,request.mode==InspectionMode::Full);
                    if(request.mode==InspectionMode::Full&&file)add(VerifyRestoredBlob{b.content.id,b.revision,file->evidence,*file->sha256});
                } });
            pages<UploadId>(c, [&](auto after) { return reader->uploads(c, PageSize::from(200).value(), after); }, [&](const Upload& u) {
                if(u.state==UploadState::Completed||((static_cast<int>(u.state)>=5)&&!u.cleanupPending))return;
                if(u.state==UploadState::Open&&!u.cancelRequested) {
                    auto f=observe(u.id,InspectionMode::Quick);
                    if(!f||f->evidence.size.value()<u.durableOffset.value()){finding(FindingCode::UploadStateInconsistent,u.id);safePlan=false;}
                    else if(f->evidence.size.value()>u.durableOffset.value()) {finding(FindingCode::UploadStateInconsistent,u.id,true);add(TruncateUnconfirmedTail{u.id,u.generation,u.durableOffset,f->evidence});}
                    return;
                }
                if(u.state==UploadState::Ready&&!u.cancelRequested) {
                    auto key=u.location==StagingLocation::Object?MaintenanceFileKey{*u.pendingBlobId}:MaintenanceFileKey{u.id};
                    auto f=observe(key,request.mode);
                    if(!f||f->evidence.size.value()!=u.original.length.value()||(request.mode==InspectionMode::Full&&f->sha256!=u.actualSha256)) {finding(FindingCode::UploadStateInconsistent,u.id);safePlan=false;}
                    return;
                }
                finding(FindingCode::MaintenancePending,u.id,true);
                auto proof=checked(c,digest_.digest(u.id.toString()+":"+std::to_string(u.generation.value())+":"+std::to_string(static_cast<int>(u.state))+":"+std::to_string(u.durableOffset.value())));
                add(ResumeUpload{u.id,u.generation,proof}); });
            pages<QuarantineId>(c, [&](auto after) { return reader->quarantine(c, PageSize::from(200).value(), after); }, [&](const QuarantineEntry& q) {
                auto f=observe(q.id,request.mode);
                if(q.state!=QuarantineState::Held||!f||!same(f->evidence,q.evidence)||(request.mode==InspectionMode::Full&&f->sha256!=q.sha256)) {finding(FindingCode::UnsafeEntry,q.id);safePlan=false;} });
            auto inventory = take(files_.inventory(c, lease, InspectionMode::Quick));
            std::size_t count = 0;
            for (;;) {
                check(c);
                auto page = take(inventory->next(c, PageSize::from(200).value()));
                if (page.empty()) break;
                if (page.size() > 200) throw failure(c, MaintenanceError::CheckIncomplete);
                for (const auto& item : page) {
                    if (++count > 100000) throw failure(c, MaintenanceError::CheckIncomplete);
                    if (const auto* f = std::get_if<MaintenanceFinding>(&item)) {
                        emit(*f);
                        safePlan = false;
                        continue;
                    }
                    const auto& f = std::get<MaintenanceFileObservation>(item);
                    bool owned = false;
                    if (const auto* id = std::get_if<QuarantineId>(&f.key)) {
                        if (!quarantine(*id)) {
                            finding(FindingCode::UnsafeEntry, f.key);
                            safePlan = false;
                        }
                        continue;
                    }
                    if (const auto* id = std::get_if<BlobId>(&f.key)) {
                        owned = bool(object(*id));
                        if (!owned) pages<UploadId>(c, [&](auto after) { return reader->uploads(c, PageSize::from(200).value(), after); }, [&](const Upload& u) {if(u.pendingBlobId&&*u.pendingBlobId==*id)owned=true; });
                    } else
                        owned = bool(upload(std::get<UploadId>(f.key)));
                    // 即使隔离已移动，源键仍有持久责任，不能再次产生新的孤儿计划。
                    pages<QuarantineId>(c, [&](auto after) { return reader->quarantine(c, PageSize::from(200).value(), after); }, [&](const QuarantineEntry& q) {if(std::visit([](auto id)->MaintenanceFileKey{return id;},q.source)==f.key)owned=true; });
                    if (!owned) {
                        finding(FindingCode::OrphanObject, f.key, true);
                        if (request.plan) {
                            auto full = observe(f.key, InspectionMode::Full);
                            if (!full || !same(full->evidence, f.evidence) || !full->sha256) throw failure(c, MaintenanceError::StalePlan);
                            auto source = std::holds_alternative<BlobId>(f.key) ? std::variant<BlobId, UploadId>{ std::get<BlobId>(f.key) } : std::variant<BlobId, UploadId>{ std::get<UploadId>(f.key) };
                            add(QuarantineOrphan{ source, checked(c, ids_.nextQuarantine()), f.evidence, *full->sha256 });
                        }
                    }
                }
            }
        }
        auto finalSummary = take(reader->summary(c));
        report.complete &= finalSummary.complete;
        report.detailsTruncated |= finalSummary.detailsTruncated;
        reader.reset();
        auto after = take(access_.captureInspection(c, lease, request.workDirectory));
        if (!same(capture->fingerprint(), after->fingerprint())) throw failure(c, MaintenanceError::StalePlan);
        FsckResult result{ identity, capture->fingerprint(), report, {} };
        if (request.plan && safePlan && !pending && report.status() != MaintenanceReportStatus::Incomplete && identity.instance && identity.schemaVersion == 5 && identity.formatVersion == 5) {
            auto order = [](const MaintenanceAction& a) {
                return std::to_string(a.payload.index()) + std::visit([](const auto& p) -> std::string {
                           using T = std::decay_t<decltype(p)>;
                           if constexpr (std::is_same_v<T, RecomputeAccounting>)
                               return "";
                           else if constexpr (std::is_same_v<T, QuarantineOrphan>)
                               return std::to_string(p.source.index()) + std::visit([](auto id) { return id.toString(); }, p.source);
                           else
                               return p.id.toString();
                       },
                                                                      a.payload);
            };
            std::sort(actions.begin(), actions.end(), [&](const auto& a, const auto& b) { return order(a) < order(b); });
            result.plan = MaintenancePlan{ checked(c, ids_.nextPlan()), *identity.instance, 5, 5, kMaintenancePlanFormat, result.source, request.mode, report, std::move(actions), {} };
            auto proofReader = take(inspection_.open(c, *capture));
            result.plan->continuation = continuation(c, *proofReader, files_, lease, digest_, *result.plan, capture->configurationFingerprint());
            if (!validateMaintenancePlan(*result.plan) || maintenancePlanBytes(*result.plan).size() > kMaintenancePlanBytes) throw failure(c, MaintenanceError::InvalidPlan, MaintenanceStage::Plan);
        }
        return result;
    } catch (const MaintenanceFailure& e) {
        return e;
    } catch (const std::exception&) {
        return failure(c, MaintenanceError::InternalError);
    }
}
}  // namespace anof
