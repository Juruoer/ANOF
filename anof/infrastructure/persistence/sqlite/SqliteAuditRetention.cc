#include <anof/infrastructure/persistence/sqlite/SqliteAuditRetention.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <anof/infrastructure/persistence/sqlite/SqliteValue.h>
namespace anof::infrastructure::sqlite {
namespace {
class Retention final : public AuditRetention {
    std::unique_ptr<SqliteConnection> db_;
    bool active_ = false;
    int nextKind_ = 0;

public:
    explicit Retention(std::unique_ptr<SqliteConnection> db) : db_(std::move(db)) {}
    Result<AuditRetentionResult> eraseExpired(const AuditRetentionRequest& r) override {
        SqliteTransactionContext ctx(*db_, active_);
        auto begin = ctx.begin();
        if (!begin) return begin.error();
        const char* sourceTables[] = { "identity_audit_events", "audit_events", "drive_audit_events", "transfer_audit_events", "maintenance_audit_events" };
        std::string candidates;
        for (int kind = 0; kind < 5; ++kind) {
            if (kind) candidates += " UNION ALL ";
            const std::string time = kind == 2 ? "observed_at" : "occurred_at";
            candidates += "SELECT * FROM (SELECT " + std::to_string(kind) + " kind,sequence id," + time +
                          " time FROM " + sourceTables[kind] + " WHERE " + time + "<?1 ORDER BY " + time + ",sequence LIMIT ?2)";
        }
        auto q = detail::query(ctx, "SELECT kind,id FROM (SELECT kind,id,time,row_number() OVER(PARTITION BY kind ORDER BY time,id) ordinal FROM (" + candidates + ") WHERE time<?1) ORDER BY ordinal,((kind+5-?3)%5),time,id LIMIT ?2", { r.before.time_since_epoch().count(), r.totalLimit.value() + 1, nextKind_ });
        if (!q) return q.error();
        std::vector<std::pair<int, std::int64_t>> ids;
        bool more = false;
        for (;;) {
            auto row = q.value()->step();
            if (!row) return ctx.fail(row.error());
            if (!row.value()) break;
            if (ids.size() == static_cast<std::size_t>(r.totalLimit.value())) {
                more = true;
                break;
            }
            auto kind = q.value()->integer(0), id = q.value()->integer(1);
            if (!kind || !id) return ctx.fail(ErrorCode::IntegrityError);
            ids.emplace_back(static_cast<int>(kind.value()), id.value());
        }
        q.value().reset();
        const char* tables[] = { "identity_audit_events", "audit_events", "drive_audit_events", "transfer_audit_events", "maintenance_audit_events" };
        for (auto [kind, id] : ids) {
            auto del = detail::one(ctx, detail::write(ctx, "DELETE FROM " + std::string(tables[kind]) + " WHERE sequence=?1", { id }));
            if (!del) return del.error();
        }
        auto commit = ctx.commit();
        if (!commit) return commit.error();
        if (!ids.empty()) nextKind_ = (ids.back().first + 1) % 5;
        return AuditRetentionResult{ NodeCount::from(static_cast<std::int64_t>(ids.size())).value(), more };
    }
    Result<AuditRetentionResult> eraseCompletedRuns(UtcTime now, AuditBatchSize limit) override {
        constexpr auto span = kCompletedMaintenanceRetentionSeconds * 1000000;
        if (now.time_since_epoch().count() < INT64_MIN + span) return AuditRetentionResult{ {}, false };
        SqliteTransactionContext ctx(*db_, active_);
        auto begin = ctx.begin();
        if (!begin) return begin.error();
        auto q = detail::query(ctx, "SELECT id FROM maintenance_runs r WHERE completed=1 AND updated_at<?1 AND NOT EXISTS(SELECT 1 FROM maintenance_actions a WHERE a.run_id=r.id AND a.state<>2) AND NOT EXISTS(SELECT 1 FROM quarantine_entries q WHERE q.plan_id=r.plan_id) AND NOT EXISTS(SELECT 1 FROM maintenance_actions a WHERE a.run_id=r.id AND ((a.kind IN (1,2) AND EXISTS(SELECT 1 FROM uploads WHERE id=json_extract(a.payload,'$[1]'))) OR (a.kind=3 AND EXISTS(SELECT 1 FROM blobs WHERE id=json_extract(a.payload,'$[1]') AND state=1)) OR (a.kind=4 AND EXISTS(SELECT 1 FROM quarantine_entries WHERE id=json_extract(a.payload,'$[2]'))) OR (a.kind IN (5,6) AND EXISTS(SELECT 1 FROM blobs WHERE id=json_extract(a.payload,'$[1]') AND state=2)))) ORDER BY updated_at,id LIMIT ?2", { now.time_since_epoch().count() - span, limit.value() + 1 });
        if (!q) return q.error();
        std::vector<std::string> ids;
        bool more = false;
        for (;;) {
            auto row = q.value()->step();
            if (!row) return ctx.fail(row.error());
            if (!row.value()) break;
            if (ids.size() == static_cast<std::size_t>(limit.value())) {
                more = true;
                break;
            }
            auto id = q.value()->text(0);
            if (!id) return ctx.fail(id.error());
            ids.push_back(id.value());
        }
        q.value().reset();
        for (const auto& id : ids) {
            auto a = detail::write(ctx, "DELETE FROM maintenance_actions WHERE run_id=?1", { id });
            if (!a) return a.error();
            auto r = detail::one(ctx, detail::write(ctx, "DELETE FROM maintenance_runs WHERE id=?1", { id }));
            if (!r) return r.error();
        }
        auto commit = ctx.commit();
        if (!commit) return commit.error();
        return AuditRetentionResult{ NodeCount::from(static_cast<std::int64_t>(ids.size())).value(), more };
    }
};
}  // namespace
Result<std::unique_ptr<AuditRetention>> openSqliteAuditRetention(const std::filesystem::path& path, SqliteOptions options) {
    auto db = openSqliteDatabase(path, options);
    if (!db) return db.error();
    return std::unique_ptr<AuditRetention>(new Retention(std::move(db).value()));
}
}  // namespace anof::infrastructure::sqlite
