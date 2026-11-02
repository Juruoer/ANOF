#include <anof/application/maintenance/internal/FsckSupport.h>
namespace anof {
using namespace maintenance;
namespace {
bool sameFact(const MaintenanceActionEvidence& a, const MaintenanceActionEvidence& b) {
    if (a.files.size() != b.files.size()) return false;
    for (const auto& f : a.files) {
        auto it = std::find_if(b.files.begin(), b.files.end(), [&](const auto& x) { return x.key == f.key; });
        if (it == b.files.end() || bool(it->file) != bool(f.file) || it->sha256 != f.sha256 || (f.file && !same(*f.file, *it->file))) return false;
    }
    return true;
}
bool sameActions(const MaintenancePlan& expected, const MaintenancePlan& generated) {
    if (expected.actions.size() != generated.actions.size()) return false;
    for (std::size_t i = 0; i < expected.actions.size(); ++i) {
        auto payload = generated.actions[i].payload;
        auto* a = std::get_if<QuarantineOrphan>(&payload);
        auto* b = std::get_if<QuarantineOrphan>(&expected.actions[i].payload);
        if (a && b) a->destination = b->destination;
        if (maintenanceActionBytes(payload) != maintenanceActionBytes(expected.actions[i].payload)) return false;
    }
    return true;
}
}  // namespace
MaintenanceResult<ApplyRepairResult> FsckService::apply(const MaintenanceContext& c, const ApplyRepairRequest& request, const MaintenancePlan& plan, const FindingSink& sink) {
    bool effects = false;
    std::optional<std::filesystem::path> artifact;
    std::optional<MaintenanceActionId> actionId;
    try {
        check(c, MaintenanceStage::Validate);
        if (!request.confirm || request.originalBackup.empty()) throw failure(c, MaintenanceError::InvalidArgument, MaintenanceStage::Validate);
        if (!validateMaintenancePlan(plan) || !plan.continuation || maintenancePlanBytes(plan).size() > kMaintenancePlanBytes) throw failure(c, MaintenanceError::InvalidPlan, MaintenanceStage::Validate);
        for (const auto& a : plan.actions)
            if (checked(c, digest_.digest(maintenanceActionBytes(a.payload))) != a.expectedPostcondition) throw failure(c, MaintenanceError::InvalidPlan, MaintenanceStage::Validate);
        auto digest = checked(c, digest_.digest(maintenancePlanBytes(plan)));
        auto lease = take(access_.acquireExisting(c, request.root));
        std::optional<MaintenanceRun> run;
        {
            auto copy = take(access_.captureInspection(c, *lease, request.workDirectory));
            auto read = take(inspection_.open(c, *copy));
            auto identity = take(read->identity(c));
            if (identity.instance != plan.instance || identity.schemaVersion != plan.schemaVersion || identity.formatVersion != plan.formatVersion) throw failure(c, MaintenanceError::StalePlan);
            pages<MaintenanceRunId>(c, [&](auto cursor) { return read->runs(c, PageSize::from(200).value(), cursor); }, [&](const MaintenanceRun& r) {
                if(r.plan==plan.id){if(run)throw failure(c,MaintenanceError::InvalidPlan);run=r;}
                else if(!r.completed)throw failure(c,MaintenanceError::MaintenancePending); });
            if (run && run->planDigest != digest) throw failure(c, MaintenanceError::InvalidPlan);
            if (!run && !same(copy->fingerprint(), plan.source)) throw failure(c, MaintenanceError::StalePlan);
            if (!same(continuation(c, *read, files_, *lease, digest_, plan, copy->configurationFingerprint()), *plan.continuation)) throw failure(c, MaintenanceError::StalePlan);
        }
        std::unique_ptr<MaintenanceDestination> destination;
        if (!run) {
            auto current = take(inspectHeld(c, *lease, { request.root, request.workDirectory, plan.mode, true }, {}));
            if (!current.plan || !sameActions(plan, *current.plan) || !same(current.source, plan.source) ||
                plan.inspection.info != current.report.info || plan.inspection.warnings != current.report.warnings || plan.inspection.errors != current.report.errors)
                throw failure(c, MaintenanceError::InvalidPlan, MaintenanceStage::Plan);
            // 非阻塞锁仍按规范绝对路径排序；重新取得源后再复核，关闭放锁窗口。
            const auto source = request.root.lexically_normal(), target = request.originalBackup.lexically_normal();
            if (!source.is_absolute() || !target.is_absolute()) throw failure(c, MaintenanceError::InvalidArgument, MaintenanceStage::AcquireLocks);
            if (target < source) {
                lease.reset();
                destination = take(access_.acquireDestination(c, source, target));
                lease = take(access_.acquireExisting(c, source));
            } else
                destination = take(access_.acquireDestination(c, source, target));
            artifact = destination->staging();
            {
                auto fresh = take(access_.captureInspection(c, *lease, request.workDirectory));
                if (!same(fresh->fingerprint(), plan.source)) throw failure(c, MaintenanceError::StalePlan);
            }
            take(files_.preserveOriginal(c, *lease, *destination, plan.source));
            artifact = destination->target();
        }
        if (run) {
            take(files_.verifyOriginal(c, request.originalBackup, plan.source));
            artifact = request.originalBackup;
        }
        auto session = take(repair_.open(c, *lease));
        if (!run) {
            const auto now = clock_.now();
            run = MaintenanceRun{ checked(c, ids_.nextRun()), plan.id, plan.instance, digest, now, now, false, {} };
            auto tx = take(session->beginWrite(c));
            take(tx->insertRun(c, *run));
            take(tx->appendAudit(c, { run->id, c.operationId, MaintenanceAuditKind::PlanStarted, MaintenanceOutcome::Confirmed, {}, {}, {}, now }));
            for (std::size_t i = 0; i < plan.actions.size(); ++i) {
                const auto& a = plan.actions[i];
                actionId = a.id;
                take(tx->prepareAction(c, run->id, { MaintenanceActionIndex::from(static_cast<std::int64_t>(i)).value(), a, MaintenanceActionState::Prepared, {}, now, now, {} }));
                take(tx->appendAudit(c, { run->id, c.operationId, MaintenanceAuditKind::ActionPrepared, MaintenanceOutcome::Confirmed, {}, {}, a.id, now }));
            }
            take(tx->commit(c));
            effects = true;
        } else
            effects = true;
        const bool completed = run->completed;
        std::size_t index = 0;
        pages<MaintenanceActionIndex>(c, [&](auto after) { return session->actions(c, run->id, PageSize::from(200).value(), after); }, [&](MaintenanceActionRecord record) {
            if(index>=plan.actions.size()||record.index.value()!=static_cast<std::int64_t>(index))throw failure(c,MaintenanceError::InvalidPlan);
            const auto& action=plan.actions[index++];actionId=action.id;
            if(record.action.id!=action.id||record.action.expectedPostcondition!=action.expectedPostcondition||maintenanceActionBytes(record.action.payload)!=maintenanceActionBytes(action.payload))throw failure(c,MaintenanceError::InvalidPlan);
            auto evidence=[&] {
                MaintenanceEvidenceBinding binding{plan.instance,run->id,plan.id,digest,action.id,record.revision};
                if(std::holds_alternative<RecomputeAccounting>(action.payload))return MaintenanceActionEvidence{binding,MaintenanceEvidenceQuality::Durable,{},action.expectedPostcondition};
                return take(files_.verifyAction(c,*lease,binding,action));
            };
            if(record.state==MaintenanceActionState::Verified) {
                auto fresh=evidence();if(!record.resultEvidence||!sameFact(fresh,*record.resultEvidence))throw failure(c,MaintenanceError::StalePlan);
                take(session->verifyCompletedAction(c,run->id,action.id,fresh));return;
            }
            if(completed)throw failure(c,MaintenanceError::IntegrityError);
            if(record.state==MaintenanceActionState::Prepared) {
                if (const auto* q=std::get_if<QuarantineOrphan>(&action.payload)) {
                    bool exists=false;
                    {
                        auto copy=take(access_.captureInspection(c,*lease,request.workDirectory));
                        auto reader=take(inspection_.open(c,*copy));
                        auto page=take(reader->quarantine(c,PageSize::from(1).value(),before(q->destination)));
                        if(!page.items.empty()&&page.items.front().id==q->destination) {
                            const auto& entry=page.items.front();
                            if(entry.plan!=plan.id||entry.action!=action.id||entry.state!=QuarantineState::Prepared||entry.source!=q->source||!same(entry.evidence,q->before)||entry.sha256!=q->sha256)throw failure(c,MaintenanceError::StalePlan);
                            exists=true;
                        }
                    }
                    if(!exists){const auto now=clock_.now();auto tx=take(session->beginWrite(c));take(tx->prepareQuarantine(c,{q->destination,q->source,q->before,q->sha256,q->before.size,QuarantineState::Prepared,plan.id,action.id,now,now,{}}));take(tx->commit(c));}
                }
                // 已完成文件效果但APPLIED未提交时，先确认当前后置事实，不能再次覆盖/截断。
                bool fileDone=false;
                if(std::holds_alternative<TruncateUnconfirmedTail>(action.payload)) {
                    auto probe=files_.verifyAction(c,*lease,{plan.instance,run->id,plan.id,digest,action.id,record.revision},action);
                    fileDone=bool(probe);
                    if(!probe&&probe.error().code==MaintenanceError::Cancelled)throw probe.error();
                }
                if(auto* payload1=std::get_if<TruncateUnconfirmedTail>(&action.payload)){if(!fileDone)take(files_.truncateTail(c,*lease,*payload1));}
                else if(auto* payload2=std::get_if<QuarantineOrphan>(&action.payload))take(files_.quarantine(c,*lease,{payload2->destination,payload2->source,payload2->before,payload2->sha256,payload2->before.size,QuarantineState::Prepared,plan.id,action.id,record.createdAt,record.updatedAt,{}}));
                else if(auto* payload3=std::get_if<ResumeUpload>(&action.payload))take(recovery_.resume(c,*lease,*payload3));
                else if(auto* payload4=std::get_if<ResumeDeletingBlob>(&action.payload))take(recovery_.remove(c,*lease,*payload4));
                auto receipt=evidence();const auto now=clock_.now();
                auto tx=take(session->beginWrite(c));
                if(auto* payload5=std::get_if<RecomputeAccounting>(&action.payload))take(tx->recomputeAccounting(c,*payload5));
                else if(auto* payload6=std::get_if<MarkBlobUnavailable>(&action.payload))take(tx->markUnavailable(c,*payload6,receipt,now));
                else if(auto* payload7=std::get_if<VerifyRestoredBlob>(&action.payload))take(tx->restoreAvailable(c,*payload7,receipt,now));
                else if(auto* payload8=std::get_if<QuarantineOrphan>(&action.payload))take(tx->holdQuarantine(c,payload8->destination,{},receipt,now));
                take(tx->advanceAction(c,run->id,action.id,record.revision,MaintenanceActionState::Applied,receipt,now));
                take(tx->appendAudit(c,{run->id,c.operationId,MaintenanceAuditKind::ActionApplied,MaintenanceOutcome::Confirmed,{},{},action.id,now}));
                take(tx->commit(c));record.state=MaintenanceActionState::Applied;record.revision=record.revision.add(1).value();record.resultEvidence=receipt;
            }
            if(auto* payload9=std::get_if<ResumeDeletingBlob>(&action.payload))take(recovery_.finalize(c,*lease,*payload9));
            auto receipt=evidence();if(record.resultEvidence&&!sameFact(receipt,*record.resultEvidence))throw failure(c,MaintenanceError::StalePlan);
            const auto now=clock_.now();auto tx=take(session->beginWrite(c));
            take(tx->advanceAction(c,run->id,action.id,record.revision,MaintenanceActionState::Verified,receipt,now));
            take(tx->appendAudit(c,{run->id,c.operationId,MaintenanceAuditKind::ActionVerified,MaintenanceOutcome::Confirmed,{},{},action.id,now}));take(tx->commit(c)); });
        if (index != plan.actions.size()) throw failure(c, MaintenanceError::IntegrityError);
        if (!completed) {
            const auto now = clock_.now();
            auto tx = take(session->beginWrite(c));
            take(tx->completeRun(c, run->id, run->revision, now));
            take(tx->appendAudit(c, { run->id, c.operationId, MaintenanceAuditKind::PlanCompleted, MaintenanceOutcome::Confirmed, {}, {}, {}, now }));
            take(tx->commit(c));
        }
        session.reset();
        auto verification = take(inspectHeld(c, *lease, { request.root, request.workDirectory, plan.mode, false }, sink));
        return ApplyRepairResult{ run->id, completed, std::move(verification) };
    } catch (MaintenanceFailure e) {
        if (effects && e.outcome == MaintenanceOutcome::NotStarted) e.outcome = MaintenanceOutcome::Confirmed;
        if (artifact && !e.artifact) e.artifact = artifact;
        if (actionId && !e.action) e.action = actionId;
        return e;
    } catch (const std::exception&) {
        auto e = failure(c, MaintenanceError::InternalError, MaintenanceStage::Recover);
        e.outcome = MaintenanceOutcome::Unknown;
        e.artifact = artifact;
        e.action = actionId;
        return e;
    }
}
}  // namespace anof
