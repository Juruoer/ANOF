#include <anof/infrastructure/persistence/sqlite/SqliteBlobRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceFiles.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMaintenanceRead.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataRepair.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUploadRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <anof/infrastructure/persistence/sqlite/internal/MaintenanceCall.h>
namespace anof::infrastructure::sqlite {
namespace {
using namespace maintenance;
class RepairTransaction final : public MetadataRepairTransaction {
    OperationId operation_;
    SqliteTransactionContext ctx_;
    std::optional<MaintenanceActionId> action_;
    MaintenanceFailure fail(const MaintenanceContext& call, const CallProgress& progress, ErrorCode e = ErrorCode::IntegrityError) {
        ctx_.fail(e);
        auto out = progress.error(call, e, MaintenanceStage::Recover);
        out.action = action_;
        return out;
    }
    MaintenanceResult<void> result(const MaintenanceContext& call, const CallProgress& progress, Result<void> r) {
        if (!r) return fail(call, progress, r.error().code);
        return {};
    }
    Result<MaintenanceActionRecord> bound(const MaintenanceActionEvidence& e) {
        action_ = e.binding.action;
        auto run = readRun(ctx_, e.binding.run);
        if (!run) return run.error();
        if (!run.value() || run.value()->completed) return ctx_.fail(ErrorCode::Conflict);
        auto action = readAction(ctx_, run.value()->id, e.binding.action);
        if (!action) return action.error();
        if (!action.value()) return ctx_.fail(ErrorCode::NotFound);
        auto& r = *run.value();
        auto& a = *action.value();
        MaintenanceEvidenceBinding binding{ r.instance, r.id, r.plan, r.planDigest, a.action.id, a.revision };
        if (!validateMaintenanceEvidence(binding, a.action, e) || a.state == MaintenanceActionState::Verified) return ctx_.fail(ErrorCode::IntegrityError);
        return a;
    }
    Result<void> predicates(const MaintenanceActionRecord& a, const MaintenanceActionEvidence& e) {
        SqliteBlobRepository blobs(ctx_);
        SqliteUploadRepository uploads(ctx_);
        if (auto* p = std::get_if<RecomputeAccounting>(&a.action.payload)) {
            auto h = accountingDigest(ctx_, false);
            if (!h) return h.error();
            if (h.value() != p->provenLedger) return ctx_.fail(ErrorCode::Conflict);
            auto recorded = verifyRecordedAccounting(ctx_);
            if (!recorded) return recorded.error();
        } else if (auto* payload1 = std::get_if<TruncateUnconfirmedTail>(&a.action.payload)) {
            auto u = uploads.find(payload1->id);
            if (!u) return u.error();
            if (!u.value() || u.value()->generation != payload1->generation || u.value()->durableOffset != payload1->confirmed) return ctx_.fail(ErrorCode::Conflict);
        } else if (auto* payload2 = std::get_if<ResumeUpload>(&a.action.payload)) {
            auto u = uploads.find(payload2->id);
            if (!u) return u.error();
            if (!u.value() || u.value()->generation.value() < payload2->generation.value()) return ctx_.fail(ErrorCode::Conflict);
            auto valid = validateMaintenanceUploadFacts(*u.value(), *payload2, e);
            if (!valid) return ctx_.fail(valid.error());
        } else if (auto* payload3 = std::get_if<ResumeDeletingBlob>(&a.action.payload)) {
            auto b = blobs.record(payload3->id);
            if (!b) return b.error();
            auto n = blobs.hasNodeReference(payload3->id), u = blobs.hasUploadReference(payload3->id);
            if (!n || !u || n.value() || u.value()) return ctx_.fail(ErrorCode::IntegrityError);
            if (a.state == MaintenanceActionState::Prepared) {
                if (!b.value() || b.value()->state != BlobState::Deleting || b.value()->revision != payload3->revision) return ctx_.fail(ErrorCode::Conflict);
            } else {
                if (b.value()) return ctx_.fail(ErrorCode::Conflict);
                auto q = detail::query(ctx_, "SELECT retained_bytes=coalesce((SELECT sum(charged_bytes) FROM blobs),0)+coalesce((SELECT sum(charged_bytes) FROM quarantine_entries),0) FROM storage_accounting WHERE singleton=1");
                if (!q) return q.error();
                auto row = q.value()->step();
                if (!row || !row.value() || q.value()->integer(0).value() != 1) return ctx_.fail(ErrorCode::IntegrityError);
            }
        } else if (auto* payload4 = std::get_if<MarkBlobUnavailable>(&a.action.payload)) {
            auto b = blobs.record(payload4->id);
            if (!b) return b.error();
            auto revised = payload4->revision.add(1);
            if (!b.value() || !revised || b.value()->revision != revised.value() || b.value()->state != BlobState::Unavailable || !b.value()->unavailable || maintenance::encode(MaintenanceActionPayload{ MarkBlobUnavailable{ payload4->id, payload4->revision, *b.value()->unavailable } }) != maintenance::encode(a.action.payload)) return ctx_.fail(ErrorCode::Conflict);
        } else if (auto* payload5 = std::get_if<VerifyRestoredBlob>(&a.action.payload)) {
            auto b = blobs.record(payload5->id);
            if (!b) return b.error();
            auto revised = payload5->revision.add(1);
            if (!b.value() || !revised || b.value()->revision != revised.value() || b.value()->state != BlobState::Available || b.value()->content.sha256 != payload5->sha256 || b.value()->content.size.value() != payload5->observed.size.value()) return ctx_.fail(ErrorCode::Conflict);
        } else if (auto* payload6 = std::get_if<QuarantineOrphan>(&a.action.payload)) {
            auto q = detail::query(ctx_, "SELECT state,plan_id,action_id,charged_bytes,source_area,source_id,evidence,sha256,revision FROM quarantine_entries WHERE id=?1", { payload6->destination.toString() });
            if (!q) return q.error();
            auto r = q.value()->step();
            if (!r || !r.value() || q.value()->integer(0).value() != 1 || q.value()->text(1).value() != e.binding.plan.toString() || q.value()->text(2).value() != a.action.id.toString() || q.value()->integer(3).value() != payload6->before.size.value()) return ctx_.fail(ErrorCode::Conflict);
            const auto source = std::visit([](auto id) { return id.toString(); }, payload6->source);
            if (q.value()->integer(4).value() != static_cast<std::int64_t>(payload6->source.index()) || q.value()->text(5).value() != source || q.value()->text(7).value() != payload6->sha256.toString() || q.value()->integer(8).value() != 2) return ctx_.fail(ErrorCode::IntegrityError);
            const MaintenanceFileFact* held = nullptr;
            for (const auto& file : e.files)
                if (file.key == MaintenanceFileKey{ payload6->destination }) held = &file;
            if (!held || !held->file) return ctx_.fail(ErrorCode::IntegrityError);
            try {
                auto stored = q.value()->boundedTextView(6, 4096);
                if (!stored || Json::parse(stored.value()) != maintenance::encode(*held->file)) return ctx_.fail(ErrorCode::IntegrityError);
            } catch (const std::exception&) {
                return ctx_.fail(ErrorCode::IntegrityError);
            }
        }
        return {};
    }

public:
    RepairTransaction(OperationId operation, SqliteConnection& db, bool& active) : operation_(operation), ctx_(db, active) {}
    Result<void> begin() { return ctx_.begin(); }
    MaintenanceResult<void> verifyCompleted(const MaintenanceContext& call, MaintenanceRunId runId, MaintenanceActionId actionId, const MaintenanceActionEvidence& evidence) {
        CallProgress progress(&ctx_.connection(), call);
        auto begun = ctx_.beginRead();
        if (!begun) return fail(call, progress, begun.error().code);
        auto run = readRun(ctx_, runId);
        auto action = readAction(ctx_, runId, actionId);
        if (!run || !run.value() || !action || !action.value() || action.value()->state != MaintenanceActionState::Verified) return fail(call, progress);
        auto& r = *run.value();
        auto& a = *action.value();
        if (!validateMaintenanceEvidence({ r.instance, r.id, r.plan, r.planDigest, a.action.id, a.revision }, a.action, evidence)) return fail(call, progress);
        if (std::holds_alternative<RecomputeAccounting>(a.action.payload)) return result(call, progress, verifyRecordedAccounting(ctx_));
        return result(call, progress, predicates(a, evidence));
    }
    MaintenanceResult<void> insertRun(const MaintenanceContext& call, const MaintenanceRun& r) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        if (r.completed || r.revision.value() != 1 || r.createdAt != r.updatedAt) return fail(call, progress);
        auto identity = detail::query(ctx_, "SELECT instance_id FROM instance_settings WHERE singleton=1");
        if (!identity) return fail(call, progress, identity.error().code);
        auto row = identity.value()->step();
        if (!row || !row.value() || identity.value()->text(0).value() != r.instance.toString()) return fail(call, progress);
        return result(call, progress, detail::one(ctx_, detail::write(ctx_, "INSERT INTO maintenance_runs VALUES(?1,?2,?3,?4,?5,?6,0,1)", { r.id.toString(), r.plan.toString(), r.instance.toString(), r.planDigest.toString(), r.createdAt.time_since_epoch().count(), r.updatedAt.time_since_epoch().count() })));
    }
    MaintenanceResult<void> prepareAction(const MaintenanceContext& call, MaintenanceRunId id, const MaintenanceActionRecord& a) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        action_ = a.action.id;
        if (a.index.value() >= static_cast<std::int64_t>(kMaintenanceActionLimit) || !validateMaintenanceActionRecord(a) || a.state != MaintenanceActionState::Prepared || a.revision.value() != 1 || a.createdAt != a.updatedAt) return fail(call, progress);
        auto run = readRun(ctx_, id);
        if (!run) return fail(call, progress, run.error().code);
        if (!run.value() || run.value()->completed || a.createdAt < run.value()->createdAt) return fail(call, progress);
        auto q = detail::query(ctx_, "SELECT count(*),coalesce(max(action_index),-1) FROM maintenance_actions WHERE run_id=?1", { id.toString() });
        if (!q) return fail(call, progress, q.error().code);
        auto row = q.value()->step();
        if (!row || !row.value() || q.value()->integer(0).value() != a.index.value() || q.value()->integer(1).value() != a.index.value() - 1) return fail(call, progress);
        const auto encoded = maintenance::encode(a.action.payload).dump();
        return result(call, progress, detail::one(ctx_, detail::write(ctx_, "INSERT INTO maintenance_actions VALUES(?1,?2,?3,?4,?5,?6,0,NULL,?7,?7,1)", { a.action.id.toString(), id.toString(), a.index.value(), static_cast<std::int64_t>(a.action.payload.index()), encoded, a.action.expectedPostcondition.toString(), a.createdAt.time_since_epoch().count() })));
    }
    MaintenanceResult<void> advanceAction(const MaintenanceContext& call, MaintenanceRunId run, MaintenanceActionId id, Revision expected, MaintenanceActionState state, const MaintenanceActionEvidence& e, UtcTime now) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        action_ = id;
        if (e.binding.run != run || e.binding.action != id || e.binding.actionRevision != expected) return fail(call, progress);
        auto a = bound(e);
        if (!a) return fail(call, progress, a.error().code);
        if (static_cast<int>(state) != static_cast<int>(a.value().state) + 1 || state == MaintenanceActionState::Prepared) return fail(call, progress);
        auto facts = predicates(a.value(), e);
        if (!facts) return fail(call, progress, facts.error().code);
        auto rev = expected.add(1);
        if (!rev) return fail(call, progress, rev.error().code);
        auto time = nondecreasingEventTime(now, a.value().createdAt, a.value().updatedAt);
        const auto encoded = maintenance::encode(e).dump();
        return result(call, progress, detail::one(ctx_, detail::write(ctx_, "UPDATE maintenance_actions SET state=?1,evidence=?2,updated_at=?3,revision=?4 WHERE id=?5 AND run_id=?6 AND revision=?7", { static_cast<std::int64_t>(state), encoded, time.time_since_epoch().count(), rev.value().value(), id.toString(), run.toString(), expected.value() })));
    }
    MaintenanceResult<void> completeRun(const MaintenanceContext& call, MaintenanceRunId id, Revision expected, UtcTime now) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        auto run = readRun(ctx_, id);
        if (!run) return fail(call, progress, run.error().code);
        if (!run.value() || run.value()->completed || run.value()->revision != expected) return fail(call, progress, ErrorCode::Conflict);
        auto rev = expected.add(1);
        if (!rev) return fail(call, progress, rev.error().code);
        auto time = nondecreasingEventTime(now, run.value()->createdAt, run.value()->updatedAt);
        return result(call, progress, detail::one(ctx_, detail::write(ctx_, "UPDATE maintenance_runs SET completed=1,revision=?1,updated_at=max(?2,coalesce((SELECT max(updated_at) FROM maintenance_actions WHERE run_id=?3),?2)) WHERE id=?3 AND revision=?4 AND NOT EXISTS(SELECT 1 FROM maintenance_actions WHERE run_id=?3 AND state<>2)", { rev.value().value(), time.time_since_epoch().count(), id.toString(), expected.value() })));
    }
    MaintenanceResult<void> markUnavailable(const MaintenanceContext& call, const MarkBlobUnavailable& p, const MaintenanceActionEvidence& e, UtcTime now) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        auto a = bound(e);
        if (!a) return fail(call, progress, a.error().code);
        if (a.value().state != MaintenanceActionState::Prepared || maintenance::encode(a.value().action.payload) != maintenance::encode(MaintenanceActionPayload{ p })) return fail(call, progress);
        SqliteBlobRepository repo(ctx_);
        auto b = repo.record(p.id);
        if (!b) return fail(call, progress, b.error().code);
        if (!b.value() || b.value()->revision != p.revision || b.value()->state == BlobState::Deleting) return fail(call, progress, ErrorCode::Conflict);
        auto charge = unavailableCharge(b.value()->content, p.evidence);
        auto rev = p.revision.add(1);
        if (!charge || !rev) return fail(call, progress);
        const auto& f = p.evidence.file;
        auto time = nondecreasingEventTime(now, b.value()->createdAt, b.value()->updatedAt);
        auto saved = detail::one(ctx_, detail::write(ctx_, "UPDATE blobs SET state=2,charged_bytes=?1,unavailable_reason=?2,observed_size=?3,observed_sha256=?4,observed_device=?5,observed_inode=?6,observed_mtime_ns=?7,observed_ctime_ns=?8,updated_at=?9,revision=?10 WHERE id=?11 AND revision=?12", { charge.value().value(), static_cast<std::int64_t>(p.evidence.reason), f ? detail::Parameter(f->size.value()) : detail::Parameter(nullptr), p.evidence.sha256 ? detail::Parameter(detail::BlobParameter{ p.evidence.sha256->bytes().data(), 32 }) : detail::Parameter(nullptr), f ? detail::Parameter(std::to_string(f->device)) : detail::Parameter(nullptr), f ? detail::Parameter(std::to_string(f->inode)) : detail::Parameter(nullptr), f ? detail::Parameter(f->mtimeNs) : detail::Parameter(nullptr), f ? detail::Parameter(f->ctimeNs) : detail::Parameter(nullptr), time.time_since_epoch().count(), rev.value().value(), p.id.toString(), p.revision.value() }));
        if (!saved) return fail(call, progress, saved.error().code);
        return result(call, progress, detail::one(ctx_, detail::write(ctx_, "UPDATE storage_accounting SET retained_bytes=retained_bytes-?1+?2,revision=revision+1 WHERE singleton=1 AND retained_bytes>=?1 AND retained_bytes-?1<=9223372036854775807-?2 AND revision<9223372036854775807", { b.value()->chargedBytes.value(), charge.value().value() })));
    }
    MaintenanceResult<void> restoreAvailable(const MaintenanceContext& call, const VerifyRestoredBlob& p, const MaintenanceActionEvidence& e, UtcTime now) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        auto a = bound(e);
        if (!a) return fail(call, progress, a.error().code);
        if (a.value().state != MaintenanceActionState::Prepared || maintenance::encode(a.value().action.payload) != maintenance::encode(MaintenanceActionPayload{ p })) return fail(call, progress);
        SqliteBlobRepository repo(ctx_);
        auto b = repo.record(p.id);
        if (!b) return fail(call, progress, b.error().code);
        if (!b.value() || b.value()->revision != p.revision || b.value()->state != BlobState::Unavailable || p.observed.size.value() != b.value()->content.size.value() || p.sha256 != b.value()->content.sha256) return fail(call, progress, ErrorCode::Conflict);
        auto rev = p.revision.add(1);
        if (!rev) return fail(call, progress, rev.error().code);
        auto time = nondecreasingEventTime(now, b.value()->createdAt, b.value()->updatedAt);
        auto saved = detail::one(ctx_, detail::write(ctx_, "UPDATE blobs SET state=0,charged_bytes=size,unavailable_reason=NULL,observed_size=NULL,observed_sha256=NULL,observed_device=NULL,observed_inode=NULL,observed_mtime_ns=NULL,observed_ctime_ns=NULL,updated_at=?1,revision=?2,unreferenced_at=CASE WHEN EXISTS(SELECT 1 FROM nodes WHERE blob_id=?3) OR EXISTS(SELECT 1 FROM uploads WHERE pending_blob_id=?3 AND (state IN (3,4) OR cleanup_pending=1)) THEN NULL ELSE coalesce(unreferenced_at,?1) END WHERE id=?3 AND revision=?4", { time.time_since_epoch().count(), rev.value().value(), p.id.toString(), p.revision.value() }));
        if (!saved) return fail(call, progress, saved.error().code);
        return result(call, progress, detail::one(ctx_, detail::write(ctx_, "UPDATE storage_accounting SET retained_bytes=retained_bytes-?1+?2,revision=revision+1 WHERE singleton=1 AND retained_bytes>=?1 AND revision<9223372036854775807", { b.value()->chargedBytes.value(), b.value()->content.size.value() })));
    }
    MaintenanceResult<void> recomputeAccounting(const MaintenanceContext& call, const RecomputeAccounting& p) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        auto before = accountingDigest(ctx_, false);
        if (!before) return fail(call, progress, before.error().code);
        if (before.value() != p.provenLedger) return fail(call, progress, ErrorCode::Conflict);
        auto changed = accountingDigest(ctx_, true);
        if (!changed) return fail(call, progress, changed.error().code);
        return {};
    }
    MaintenanceResult<void> prepareQuarantine(const MaintenanceContext& call, const QuarantineEntry& q) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        if (!validateQuarantineEntry(q) || q.state != QuarantineState::Prepared || q.revision.value() != 1 || q.updatedAt != q.createdAt) return fail(call, progress);
        auto run = detail::query(ctx_, "SELECT id FROM maintenance_runs WHERE plan_id=?1 AND completed=0", { q.plan.toString() });
        if (!run) return fail(call, progress, run.error().code);
        auto r = run.value()->step();
        if (!r || !r.value()) return fail(call, progress);
        auto id = readId<MaintenanceRunId>(*run.value(), 0);
        if (!id) return fail(call, progress);
        auto a = readAction(ctx_, id.value(), q.action);
        if (!a || !a.value() || a.value()->state != MaintenanceActionState::Prepared) return fail(call, progress);
        const auto* action = std::get_if<QuarantineOrphan>(&a.value()->action.payload);
        if (!action || action->source != q.source || action->destination != q.id || action->sha256 != q.sha256 || maintenance::encode(action->before) != maintenance::encode(q.evidence)) return fail(call, progress);
        auto source = std::visit([](const auto& x) { return x.toString(); }, q.source);
        auto exists = detail::query(ctx_, q.source.index() == 0 ? "SELECT 1 FROM blobs WHERE id=?1 UNION ALL SELECT 1 FROM uploads WHERE pending_blob_id=?1" : "SELECT 1 FROM uploads WHERE id=?1", { source });
        if (!exists) return fail(call, progress, exists.error().code);
        auto found = exists.value()->step();
        if (!found || found.value()) return fail(call, progress);
        auto encoded = maintenance::encode(q.evidence).dump();
        auto saved = detail::one(ctx_, detail::write(ctx_, "INSERT INTO quarantine_entries VALUES(?1,?2,?3,?4,?5,?6,0,?7,?8,?9,?9,1)", { q.id.toString(), static_cast<std::int64_t>(q.source.index()), source, encoded, q.sha256.toString(), q.chargedBytes.value(), q.plan.toString(), q.action.toString(), q.createdAt.time_since_epoch().count() }));
        if (!saved) return fail(call, progress, saved.error().code);
        return result(call, progress, detail::one(ctx_, detail::write(ctx_, "UPDATE storage_accounting SET retained_bytes=retained_bytes+?1,revision=revision+1 WHERE singleton=1 AND retained_bytes<=9223372036854775807-?1-reserved_bytes AND revision<9223372036854775807", { q.chargedBytes.value() })));
    }
    MaintenanceResult<void> holdQuarantine(const MaintenanceContext& call, QuarantineId id, Revision rev, const MaintenanceActionEvidence& e, UtcTime now) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        auto a = bound(e);
        if (!a) return fail(call, progress, a.error().code);
        auto* p = std::get_if<QuarantineOrphan>(&a.value().action.payload);
        if (!p || p->destination != id || a.value().state != MaintenanceActionState::Prepared) return fail(call, progress);
        auto next = rev.add(1);
        if (!next) return fail(call, progress);
        const FileEvidence* held = nullptr;
        for (const auto& file : e.files)
            if (file.key == MaintenanceFileKey{ id } && file.file) held = &*file.file;
        if (!held) return fail(call, progress);
        // rename会改变ctime；HELD保存目标的新证据，原始证据仍留在动作计划中。
        return result(call, progress, detail::one(ctx_, detail::write(ctx_, "UPDATE quarantine_entries SET state=1,revision=?1,updated_at=max(created_at,updated_at,?2),evidence=?7 WHERE id=?3 AND revision=?4 AND state=0 AND plan_id=?5 AND action_id=?6", { next.value().value(), now.time_since_epoch().count(), id.toString(), rev.value(), e.binding.plan.toString(), e.binding.action.toString(), maintenance::encode(*held).dump() })));
    }
    MaintenanceResult<void> appendAudit(const MaintenanceContext& call, const MaintenanceAuditEvent& e) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        action_ = e.action;
        if (e.operation != operation_) return fail(call, progress);
        auto run = readRun(ctx_, e.run);
        if (!run || !run.value()) return fail(call, progress);
        auto eventTime = nondecreasingEventTime(e.occurredAt, run.value()->createdAt, run.value()->updatedAt);
        if (e.action) {
            auto action = readAction(ctx_, e.run, *e.action);
            if (!action || !action.value()) return fail(call, progress);
            eventTime = nondecreasingEventTime(eventTime, action.value()->createdAt, action.value()->updatedAt);
        }
        return result(call, progress, detail::one(ctx_, detail::write(ctx_, "INSERT INTO maintenance_audit_events(actor,run_id,operation_id,kind,outcome,error,resource_area,resource_id,action_id,occurred_at) VALUES('local_operator',?1,?2,?3,?4,?5,?6,?7,?8,?9)", { e.run.toString(), e.operation.toString(), static_cast<std::int64_t>(e.kind), static_cast<std::int64_t>(e.outcome), e.error ? detail::Parameter(static_cast<std::int64_t>(*e.error)) : detail::Parameter(nullptr), e.resource ? detail::Parameter(static_cast<std::int64_t>(e.resource->index())) : detail::Parameter(nullptr), e.resource ? detail::Parameter(std::visit([](const auto& v) { return v.toString(); }, *e.resource)) : detail::Parameter(nullptr), e.action ? detail::Parameter(e.action->toString()) : detail::Parameter(nullptr), eventTime.time_since_epoch().count() })));
    }
    MaintenanceResult<void> commit(const MaintenanceContext& call) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) {
            ctx_.fail(ErrorCode::InvalidArgument);
            return valid.error();
        }
        CallProgress progress(&ctx_.connection(), call);
        bool cancelledBeforeCommit = false;
        auto committed = ctx_.commit([&]() -> Result<void> {
            // 不让 COMMIT 或失败清理继续访问取消标志；提交启动后以真实结果为准。
            progress.clear();
            cancelledBeforeCommit = call.cancelled.load();
            return cancelledBeforeCommit ? Result<void>(ErrorCode::InvalidArgument) : Result<void>{};
        });
        if (!committed && cancelledBeforeCommit && committed.error().code == ErrorCode::InvalidArgument) {
            auto out = failure(call, committed.error().code);
            out.code = MaintenanceError::Cancelled;
            return out;
        }
        return result(call, progress, committed);
    }
};
class RepairSession final : public MetadataRepairSession {
    OperationId operation_;
    std::unique_ptr<detail::InstanceDatabase> db_;
    bool active_ = false;

public:
    RepairSession(const MaintenanceContext& c, std::unique_ptr<detail::InstanceDatabase> db) : operation_(c.operationId), db_(std::move(db)) {}
    MaintenanceResult<std::optional<MaintenanceRun>> findRun(const MaintenanceContext& call, MaintenancePlanId id) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) return valid.error();
        SqliteTransactionContext c(*db_->connection, active_);
        CallProgress progress(db_->connection.get(), call);
        auto begun = c.beginRead();
        if (!begun) return progress.error(call, begun.error().code, MaintenanceStage::Recover);
        auto q = detail::query(c, "SELECT id FROM maintenance_runs WHERE plan_id=?1", { id.toString() });
        if (!q) return progress.error(call, q.error().code, MaintenanceStage::Recover);
        auto row = q.value()->step();
        if (!row) return progress.error(call, row.error().code, MaintenanceStage::Recover);
        if (!row.value()) return std::optional<MaintenanceRun>{};
        auto key = readId<MaintenanceRunId>(*q.value(), 0);
        if (!key) return progress.error(call, key.error().code, MaintenanceStage::Recover);
        auto r = readRun(c, key.value());
        if (!r) return progress.error(call, r.error().code, MaintenanceStage::Recover);
        return r.value();
    }
    MaintenanceResult<Page<MaintenanceActionRecord, MaintenanceActionIndex>> actions(const MaintenanceContext& call, MaintenanceRunId id, PageSize size, std::optional<MaintenanceActionIndex> after) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) return valid.error();
        SqliteTransactionContext c(*db_->connection, active_);
        CallProgress progress(db_->connection.get(), call);
        auto begun = c.beginRead();
        if (!begun) return progress.error(call, begun.error().code, MaintenanceStage::Recover);
        auto q = detail::query(c, "SELECT id FROM maintenance_actions WHERE run_id=?1 AND action_index>?2 ORDER BY action_index LIMIT ?3", { id.toString(), after ? after->value() : -1, size.value() + 1 });
        if (!q) return progress.error(call, q.error().code, MaintenanceStage::Recover);
        Page<MaintenanceActionRecord, MaintenanceActionIndex> page;
        for (;;) {
            auto row = q.value()->step();
            if (!row) return progress.error(call, row.error().code, MaintenanceStage::Recover);
            if (!row.value()) return page;
            if (page.items.size() == static_cast<std::size_t>(size.value())) {
                page.next = page.items.back().index;
                return page;
            }
            auto key = readId<MaintenanceActionId>(*q.value(), 0);
            if (!key) return progress.error(call, key.error().code, MaintenanceStage::Recover);
            auto a = readAction(c, id, key.value());
            if (!a || !a.value()) return progress.error(call, ErrorCode::IntegrityError, MaintenanceStage::Recover);
            page.items.push_back(*a.value());
        }
    }
    MaintenanceResult<void> verifyCompletedAction(const MaintenanceContext& call, MaintenanceRunId run, MaintenanceActionId action, const MaintenanceActionEvidence& evidence) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) return valid.error();
        RepairTransaction read(operation_, *db_->connection, active_);
        return read.verifyCompleted(call, run, action, evidence);
    }
    MaintenanceResult<std::unique_ptr<MetadataRepairTransaction>> beginWrite(const MaintenanceContext& call) override {
        auto valid = validateMaintenanceCall(call, operation_, MaintenanceStage::Recover);
        if (!valid) return valid.error();
        CallProgress progress(db_->connection.get(), call);
        auto tx = std::make_unique<RepairTransaction>(operation_, *db_->connection, active_);
        auto begun = tx->begin();
        if (!begun) return progress.error(call, begun.error().code, MaintenanceStage::Recover);
        return std::unique_ptr<MetadataRepairTransaction>(std::move(tx));
    }
};
class Repair final : public MetadataRepair {
    MaintenanceResult<std::unique_ptr<MetadataRepairSession>> open(const MaintenanceContext& c, const InstanceLease& lease) override {
        if (c.cancelled.load()) return MaintenanceFailure{ MaintenanceError::Cancelled, MaintenanceStage::AcquireLocks, c.operationId, MaintenanceOutcome::NotStarted, MaintenanceReportStatus::Incomplete, {}, {} };
        auto db = detail::openInstanceDatabase(lease, {}, false);
        if (!db) return failure(c, ErrorCode::IntegrityError);
        CallProgress progress(db.value()->connection.get(), c);
        auto schema = verifySqliteSchema(*db.value()->connection);
        if (!schema) return progress.error(c, schema.error().code, MaintenanceStage::Recover);
        return std::unique_ptr<MetadataRepairSession>(new RepairSession(c, std::move(db).value()));
    }
};
}  // namespace
Result<std::unique_ptr<MetadataRepair>> makeSqliteMetadataRepair() {
    try {
        return std::unique_ptr<MetadataRepair>(new Repair);
    } catch (const std::bad_alloc&) {
        return ErrorCode::InternalError;
    }
}
}  // namespace anof::infrastructure::sqlite
