#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/internal/RepairInvariant.h>
#include <sodium.h>

#include <set>
namespace anof::infrastructure::sqlite::maintenance {
namespace {
std::string quoted(std::string s) { return "\"" + s + "\""; }
std::string list(const std::set<std::string>& ids) {
    // nil UUID is outside the accepted key domain, including for an empty list.
    std::string s = "('00000000-0000-0000-0000-000000000000'";
    for (const auto& id : ids) s += ",'" + id + "'";
    return s + ")";
}
}  // namespace
Result<Sha256> repairInvariant(SqliteTransactionContext& ctx, const MaintenancePlan& plan) {
    std::set<std::string> uploads, deleted, changed, quarantine;
    bool ledger = false;
    for (const auto& a : plan.actions) std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, ResumeUpload>)
            uploads.insert(p.id.toString());
        else if constexpr (std::is_same_v<T, ResumeDeletingBlob>)
            deleted.insert(p.id.toString());
        else if constexpr (std::is_same_v<T, MarkBlobUnavailable> || std::is_same_v<T, VerifyRestoredBlob>)
            changed.insert(p.id.toString());
        else if constexpr (std::is_same_v<T, QuarantineOrphan>)
            quarantine.insert(p.destination.toString());
        else if constexpr (std::is_same_v<T, RecomputeAccounting>)
            ledger = true;
    },
                                                  a.payload);
    const auto resume = list(uploads), gone = list(deleted), edited = list(changed), held = list(quarantine);
    const auto run = "(SELECT id FROM maintenance_runs WHERE plan_id='" + plan.id.toString() + "')";
    const auto newBlobs = "(SELECT pending_blob_id FROM uploads WHERE id IN " + resume + " AND pending_blob_id IS NOT NULL)";
    crypto_hash_sha256_state hash;
    crypto_hash_sha256_init(&hash);
    auto append = [&](std::string_view bytes) {auto prefix=std::to_string(bytes.size())+":";crypto_hash_sha256_update(&hash,reinterpret_cast<const unsigned char*>(prefix.data()),prefix.size());crypto_hash_sha256_update(&hash,reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size()); };
    auto tables = detail::query(ctx, "SELECT name,sql FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
    if (!tables) return tables.error();
    for (;;) {
        auto row = tables.value()->step();
        if (!row) return row.error();
        if (!row.value()) break;
        auto name = tables.value()->text(0);
        auto schema = tables.value()->boundedTextView(1, 32768);
        if (!name || !schema) return ErrorCode::IntegrityError;
        const auto& table = name.value();
        if (table.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789") != std::string::npos) return ErrorCode::IntegrityError;
        append(table);
        append(schema.value());
        auto columns = detail::query(ctx, "PRAGMA table_info(" + quoted(table) + ")");
        if (!columns) return columns.error();
        std::vector<std::string> expr;
        for (;;) {
            auto col = columns.value()->step();
            if (!col) return col.error();
            if (!col.value()) break;
            auto n = columns.value()->text(1);
            if (!n) return n.error();
            const auto& key = n.value();
            if (key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789") != std::string::npos) return ErrorCode::IntegrityError;
            auto value = quoted(key);
            if (table == "storage_accounting" && (key == "retained_bytes" || key == "reserved_bytes" || key == "revision")) value = "NULL";
            if ((ledger || !uploads.empty()) && ((table == "node_accounting") || table == "space_quotas" || table == "public_contribution_quotas") && (key == "used_bytes" || key == "used_nodes" || key == "reserved_bytes" || key == "reserved_nodes" || key == "revision")) value = "NULL";
            if (table == "blobs" && key != "id" && key != "size" && key != "sha256" && key != "storage_key" && key != "created_at" && key != "unreferenced_at") value = "CASE WHEN id IN " + edited + " THEN NULL ELSE " + value + " END";
            if (table == "nodes" && !uploads.empty() && (key == "revision" || key == "updated_at")) {
                const auto parent = "id IN (SELECT parent_id FROM uploads WHERE id IN " + resume + ")";
                value = "CASE WHEN " + parent + " THEN " + (key == "updated_at" ? std::string("NULL") : "revision-(SELECT count(*) FROM uploads u WHERE u.id IN " + resume + " AND u.state=5 AND u.parent_id=nodes.id)") + " ELSE " + value + " END";
            }
            if (table == "uploads") {
                const std::set<std::string> fixed = { "id", "user_id", "client_upload_id", "space_id", "original_parent_id", "original_name", "length", "expected_sha256", "parent_id", "name", "created_at" };
                if (!fixed.count(key)) value = "CASE WHEN id IN " + resume + " THEN NULL ELSE " + value + " END";
            }
            expr.push_back("quote(" + value + ")");
        }
        std::string where = "1";
        if (table == "maintenance_runs") where = "plan_id!='" + plan.id.toString() + "'";
        if (table == "maintenance_actions" || table == "maintenance_audit_events") where = "run_id NOT IN " + run;
        if (table == "quarantine_entries") where = "id NOT IN " + held;
        if (table == "blobs") where = "id NOT IN " + gone + " AND id NOT IN " + newBlobs;
        if (table == "nodes") where = "id NOT IN (SELECT result_node_id FROM uploads WHERE id IN " + resume + " AND result_node_id IS NOT NULL)";
        if (table == "transfer_audit_events") where = "upload_id IS NULL OR upload_id NOT IN " + resume;
        if (table == "audit_events") where = "request_id NOT IN " + resume;
        if (table == "drive_audit_events") where = "NOT (maintenance=1 AND first_id IN " + gone + ")";
        std::string sql = "SELECT ";
        for (std::size_t i = 0; i < expr.size(); ++i) {
            if (i) sql += ",";
            sql += expr[i];
        }
        sql += " FROM " + quoted(table) + " WHERE " + where + " ORDER BY ";
        for (std::size_t i = 0; i < expr.size(); ++i) {
            if (i) sql += ",";
            sql += std::to_string(i + 1);
        }
        auto rows = detail::query(ctx, sql);
        if (!rows) return rows.error();
        std::size_t count = 0;
        for (;;) {
            auto r = rows.value()->step();
            if (!r) return r.error();
            if (!r.value()) break;
            if (++count > 1000000) return ErrorCode::IntegrityError;
            append("row");
            for (std::size_t i = 0; i < expr.size(); ++i) {
                auto text = rows.value()->boundedTextView(static_cast<int>(i), 262144);
                if (!text) return text.error();
                append(text.value());
            }
        }
    }
    std::array<std::uint8_t, 32> out{};
    crypto_hash_sha256_final(&hash, out.data());
    return Sha256(out);
}
}  // namespace anof::infrastructure::sqlite::maintenance
