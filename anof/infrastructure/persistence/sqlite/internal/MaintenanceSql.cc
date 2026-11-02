#include <anof/infrastructure/persistence/sqlite/SqliteDriveIntegrity.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
#include <anof/infrastructure/persistence/sqlite/internal/MaintenanceSql.h>
#include <sodium.h>
namespace anof::infrastructure::sqlite::maintenance {
MaintenanceFailure failure(const MaintenanceContext& c, ErrorCode code, MaintenanceStage stage) {
    auto error = MaintenanceError::IntegrityError;
    auto outcome = MaintenanceOutcome::NotStarted;
    if (code == ErrorCode::CommitOutcomeUnknown) {
        error = MaintenanceError::CommitOutcomeUnknown;
        outcome = MaintenanceOutcome::Unknown;
    } else if (code == ErrorCode::Retryable)
        error = MaintenanceError::Busy;
    else if (code == ErrorCode::StorageFull)
        error = MaintenanceError::StorageFull;
    else if (code == ErrorCode::InvalidArgument)
        error = MaintenanceError::InvalidArgument;
    else if (code == ErrorCode::Conflict || code == ErrorCode::PreconditionFailed)
        error = MaintenanceError::StalePlan;
    else if (code == ErrorCode::InternalError)
        error = MaintenanceError::InternalError;
    return { error, stage, c.operationId, outcome, MaintenanceReportStatus::Incomplete, {}, {} };
}
Result<std::optional<MaintenanceRun>> readRun(SqliteTransactionContext& c, MaintenanceRunId id) {
    auto q = detail::query(c, "SELECT id,plan_id,instance_id,plan_digest,created_at,updated_at,completed,revision FROM maintenance_runs WHERE id=?1", { id.toString() });
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return c.fail(row.error());
    if (!row.value()) return std::optional<MaintenanceRun>{};
    auto& s = *q.value();
    auto rid = readId<MaintenanceRunId>(s, 0);
    auto plan = readId<MaintenancePlanId>(s, 1);
    auto instance = readId<InstanceId>(s, 2);
    auto hash = s.text(3);
    auto ct = readTime(s, 4), ut = readTime(s, 5);
    auto complete = readBoolean(s, 6);
    auto rev = readQuantity<Revision>(s, 7);
    if (!rid || !plan || !instance || !hash || !ct || !ut || !complete || !rev || ut.value() < ct.value()) return c.fail(ErrorCode::IntegrityError);
    auto digest = Sha256::parse(hash.value());
    if (!digest) return c.fail(ErrorCode::IntegrityError);
    return std::optional<MaintenanceRun>{ { rid.value(), plan.value(), instance.value(), digest.value(), ct.value(), ut.value(), complete.value(), rev.value() } };
}
Result<std::optional<MaintenanceActionRecord>> readAction(SqliteTransactionContext& c, MaintenanceRunId run, MaintenanceActionId id) {
    auto q = detail::query(c, "SELECT action_index,payload,expected_postcondition,state,evidence,created_at,updated_at,revision,kind FROM maintenance_actions WHERE run_id=?1 AND id=?2", { run.toString(), id.toString() });
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return c.fail(row.error());
    if (!row.value()) return std::optional<MaintenanceActionRecord>{};
    try {
        auto& s = *q.value();
        auto index = readQuantity<MaintenanceActionIndex>(s, 0).value();
        auto serializedPayload = Json::parse(s.boundedTextView(1, 16384).value());
        auto p = payload(serializedPayload);
        if (encode(p) != serializedPayload) return c.fail(ErrorCode::IntegrityError);
        auto hash = Sha256::parse(s.text(2).value()).value();
        auto state = s.integer(3).value();
        if (state < 0 || state > 2 || static_cast<std::int64_t>(p.index()) != s.integer(8).value()) return c.fail(ErrorCode::IntegrityError);
        MaintenanceActionRecord a{ index, { id, p, hash }, static_cast<MaintenanceActionState>(state), {}, readTime(s, 5).value(), readTime(s, 6).value(), readQuantity<Revision>(s, 7).value() };
        if (!s.isNull(4).value()) {
            auto serializedEvidence = Json::parse(s.boundedTextView(4, 16384).value());
            a.resultEvidence = evidence(serializedEvidence);
            if (encode(*a.resultEvidence) != serializedEvidence) return c.fail(ErrorCode::IntegrityError);
        }
        if (!validateMaintenanceActionRecord(a)) return c.fail(ErrorCode::IntegrityError);
        if (a.resultEvidence) {
            auto runRecord = readRun(c, run);
            if (!runRecord || !runRecord.value()) return c.fail(ErrorCode::IntegrityError);
            const auto& r = *runRecord.value();
            MaintenanceEvidenceBinding expected{ r.instance, r.id, r.plan, r.planDigest, id, a.resultEvidence->binding.actionRevision };
            if (!validateMaintenanceEvidence(expected, a.action, *a.resultEvidence)) return c.fail(ErrorCode::IntegrityError);
        }

        return std::optional<MaintenanceActionRecord>{ a };
    } catch (const std::exception&) {
        return c.fail(ErrorCode::IntegrityError);
    }
}
Result<void> verifyRecordedAccounting(SqliteTransactionContext& c) {
    auto q = detail::query(c, R"SQL(SELECT
      NOT EXISTS(SELECT 1 FROM space_quotas q WHERE
        used_bytes != (SELECT coalesce(sum(b.size),0) FROM nodes n JOIN blobs b ON b.id=n.blob_id WHERE n.space_id=q.space_id) OR
        used_nodes != (SELECT count(*) FROM nodes n WHERE n.space_id=q.space_id) OR
        reserved_bytes != (SELECT coalesce(sum(length),0) FROM uploads u WHERE u.space_id=q.space_id AND u.state<5) OR
        reserved_nodes != (SELECT count(*) FROM uploads u WHERE u.space_id=q.space_id AND u.state<5))
      AND NOT EXISTS(SELECT 1 FROM public_contribution_quotas q WHERE
        used_bytes != (SELECT coalesce(sum(b.size),0) FROM nodes n JOIN blobs b ON b.id=n.blob_id WHERE n.space_id=q.space_id AND n.created_by=q.user_id) OR
        used_nodes != (SELECT count(*) FROM nodes n WHERE n.space_id=q.space_id AND n.created_by=q.user_id) OR
        reserved_bytes != (SELECT coalesce(sum(length),0) FROM uploads u WHERE u.space_id=q.space_id AND u.user_id=q.user_id AND u.state<5) OR
        reserved_nodes != (SELECT count(*) FROM uploads u WHERE u.space_id=q.space_id AND u.user_id=q.user_id AND u.state<5))
      AND (SELECT retained_bytes FROM storage_accounting WHERE singleton=1)=coalesce((SELECT sum(charged_bytes) FROM blobs),0)+coalesce((SELECT sum(charged_bytes) FROM quarantine_entries),0)
      AND (SELECT reserved_bytes FROM storage_accounting WHERE singleton=1)=coalesce((SELECT sum(length) FROM uploads WHERE state<5 OR cleanup_pending=1),0)
      AND (SELECT used_nodes FROM node_accounting WHERE singleton=1)=(SELECT count(*) FROM nodes)
      AND (SELECT reserved_nodes FROM node_accounting WHERE singleton=1)=(SELECT count(*) FROM uploads WHERE state<5))SQL");
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return c.fail(row.error());
    auto valid = q.value()->integer(0);
    if (!row.value() || !valid || valid.value() != 1) return ErrorCode::IntegrityError;
    return {};
}
// 固定字段与排序编码构成账本摘要；SQLite SUM 遇溢出失败，不转浮点继续。
Result<Sha256> accountingDigest(SqliteTransactionContext& c, bool update) {
    auto shape = verifyDriveMembership(c.connection());
    if (!shape) return c.fail(shape.error());
    auto topology = detail::query(c, "WITH RECURSIVE active(id,depth) AS (SELECT root_node_id,0 FROM spaces UNION ALL SELECT n.id,a.depth+1 FROM active a JOIN nodes n ON n.parent_id=a.id WHERE n.trashed=0 AND a.depth<128) SELECT (SELECT count(*) FROM active)=(SELECT count(*) FROM nodes WHERE trashed=0) AND NOT EXISTS(SELECT 1 FROM spaces s WHERE NOT EXISTS(SELECT 1 FROM space_quotas q WHERE q.space_id=s.id)) AND NOT EXISTS(SELECT 1 FROM nodes n JOIN spaces s ON s.id=n.space_id WHERE s.kind=1 AND NOT EXISTS(SELECT 1 FROM public_contribution_quotas q WHERE q.space_id=s.id AND q.user_id=n.created_by))");
    if (!topology) return topology.error();
    auto consistent = topology.value()->step();
    if (!consistent || !consistent.value() || topology.value()->integer(0).value() != 1) return c.fail(ErrorCode::IntegrityError);
    const char* statements[] = {
        "SELECT s.id,coalesce(sum(CASE WHEN n.kind=1 THEN b.size ELSE 0 END),0),count(n.id),(SELECT coalesce(sum(length),0) FROM uploads u WHERE u.space_id=s.id AND u.state<5),(SELECT count(*) FROM uploads u WHERE u.space_id=s.id AND u.state<5) FROM spaces s LEFT JOIN nodes n ON n.space_id=s.id LEFT JOIN blobs b ON b.id=n.blob_id GROUP BY s.id ORDER BY s.id",
        "SELECT q.space_id,q.user_id,coalesce(sum(CASE WHEN n.kind=1 THEN b.size ELSE 0 END),0),count(n.id),(SELECT coalesce(sum(length),0) FROM uploads u WHERE u.space_id=q.space_id AND u.user_id=q.user_id AND u.state<5),(SELECT count(*) FROM uploads u WHERE u.space_id=q.space_id AND u.user_id=q.user_id AND u.state<5) FROM public_contribution_quotas q LEFT JOIN nodes n ON n.space_id=q.space_id AND n.created_by=q.user_id LEFT JOIN blobs b ON b.id=n.blob_id GROUP BY q.space_id,q.user_id ORDER BY q.space_id,q.user_id",
        "SELECT coalesce((SELECT sum(charged_bytes) FROM blobs),0),coalesce((SELECT sum(charged_bytes) FROM quarantine_entries),0),coalesce((SELECT sum(length) FROM uploads WHERE state<5 OR cleanup_pending=1),0),(SELECT count(*) FROM nodes),(SELECT count(*) FROM uploads WHERE state<5)"
    };
    crypto_hash_sha256_state hash;
    crypto_hash_sha256_init(&hash);
    for (int table = 0; table < 3; ++table) {
        auto q = detail::query(c, statements[table]);
        if (!q) return q.error();
        for (;;) {
            auto row = q.value()->step();
            if (!row) return c.fail(row.error());
            if (!row.value()) break;
            Json values = Json::array();
            values.push_back(table);
            auto& s = *q.value();
            int names = table == 0 ? 1 : table == 1 ? 2
                                                    : 0;
            int columns = table == 0 ? 5 : table == 1 ? 6
                                                      : 5;
            for (int i = 0; i < columns; ++i) {
                if (i < names) {
                    auto v = s.text(i);
                    if (!v) return c.fail(v.error());
                    values.push_back(v.value());
                } else {
                    auto v = readQuantity<ByteCount>(s, i);
                    if (!v) return c.fail(v.error());
                    values.push_back(v.value().value());
                }
            }
            auto text = values.dump() + "\n";
            crypto_hash_sha256_update(&hash, reinterpret_cast<const unsigned char*>(text.data()), text.size());
            if (update) {
                Result<std::int64_t> saved = ErrorCode::InternalError;
                if (table == 0)
                    saved = detail::write(c, "UPDATE space_quotas SET used_bytes=?1,used_nodes=?2,reserved_bytes=?3,reserved_nodes=?4,revision=revision+1 WHERE space_id=?5 AND revision<9223372036854775807", { s.integer(1).value(), s.integer(2).value(), s.integer(3).value(), s.integer(4).value(), s.text(0).value() });
                else if (table == 1)
                    saved = detail::write(c, "UPDATE public_contribution_quotas SET used_bytes=?1,used_nodes=?2,reserved_bytes=?3,reserved_nodes=?4,revision=revision+1 WHERE space_id=?5 AND user_id=?6 AND revision<9223372036854775807", { s.integer(2).value(), s.integer(3).value(), s.integer(4).value(), s.integer(5).value(), s.text(0).value(), s.text(1).value() });
                else {
                    auto sum = ByteCount::from(s.integer(0).value()).value().add(s.integer(1).value());
                    if (!sum) return c.fail(sum.error());
                    saved = detail::write(c, "UPDATE storage_accounting SET retained_bytes=?1,reserved_bytes=?2,revision=revision+1 WHERE singleton=1 AND revision<9223372036854775807", { sum.value().value(), s.integer(2).value() });
                    auto n = detail::one(c, detail::write(c, "UPDATE node_accounting SET used_nodes=?1,reserved_nodes=?2,revision=revision+1 WHERE singleton=1 AND revision<9223372036854775807", { s.integer(3).value(), s.integer(4).value() }));
                    if (!n) return n.error();
                }
                auto result = detail::one(c, saved);
                if (!result) return result.error();
            }
        }
    }
    std::array<std::uint8_t, 32> out{};
    crypto_hash_sha256_final(&hash, out.data());
    return Sha256(out);
}
}  // namespace anof::infrastructure::sqlite::maintenance
