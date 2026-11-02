#include <anof/infrastructure/persistence/sqlite/SqliteMaintenanceStateReader.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/internal/MaintenanceCodec.h>

namespace anof::infrastructure::sqlite {
Result<std::optional<QuarantineEntry>> SqliteMaintenanceStateReader::findQuarantine(QuarantineId id) {
    auto q = detail::query(context_, "SELECT id,source_area,source_id,evidence,sha256,charged_bytes,state,plan_id,action_id,created_at,updated_at,revision FROM quarantine_entries WHERE id=?1", { id.toString() });
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return std::optional<QuarantineEntry>{};
    try {
        auto& s = *q.value();
        using maintenance::Json;
        Json j = Json::array({ s.text(0).value(), Json::array({ s.integer(1).value(), s.text(2).value() }),
            Json::parse(s.boundedTextView(3, 4096).value()), s.text(4).value(), s.integer(5).value(), s.integer(6).value(),
            s.text(7).value(), s.text(8).value(), s.integer(9).value(), s.integer(10).value(), s.integer(11).value() });
        auto entry = maintenance::quarantine(j);
        if (!validateQuarantineEntry(entry) || entry.id != id) return context_.fail(ErrorCode::IntegrityError);
        row = s.step();
        if (!row) return context_.fail(row.error());
        if (row.value()) return context_.fail(ErrorCode::IntegrityError);
        return std::optional<QuarantineEntry>{ entry };
    } catch (const std::exception&) {
        return context_.fail(ErrorCode::IntegrityError);
    }
}
Result<bool> SqliteMaintenanceStateReader::hasPendingRuns() {
    auto q = detail::query(context_, "SELECT EXISTS(SELECT 1 FROM maintenance_runs WHERE completed=0) OR EXISTS(SELECT 1 FROM quarantine_entries WHERE state=0)");
    if (!q) return q.error();
    auto row = q.value()->step();
    if (!row) return context_.fail(row.error());
    if (!row.value()) return context_.fail(ErrorCode::IntegrityError);
    auto value = q.value()->integer(0);
    if (!value || (value.value() != 0 && value.value() != 1)) return context_.fail(ErrorCode::IntegrityError);
    return value.value() == 1;
}
}  // namespace anof::infrastructure::sqlite
