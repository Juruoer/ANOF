#pragma once
#include <anof/application/maintenance/Fsck.h>
namespace anof::maintenance {
inline MaintenanceFailure failure(const MaintenanceContext& c, MaintenanceError e, MaintenanceStage stage = MaintenanceStage::Inspect) {
    return { e, stage, c.operationId, MaintenanceOutcome::NotStarted, MaintenanceReportStatus::Incomplete, {}, {} };
}
template <class T>
T take(MaintenanceResult<T> r) {
    if (!r) throw r.error();
    return std::move(r).value();
}
inline void take(MaintenanceResult<void> r) {
    if (!r) throw r.error();
}
template <class T>
T checked(const MaintenanceContext& c, Result<T> r) {
    if (!r) throw failure(c, MaintenanceError::InternalError);
    return std::move(r).value();
}
inline void check(const MaintenanceContext& c, MaintenanceStage stage = MaintenanceStage::Inspect) { take(validateMaintenanceCall(c, c.operationId, stage)); }
inline bool same(const FileEvidence& a, const FileEvidence& b) { return a.device == b.device && a.inode == b.inode && a.size == b.size && a.mtimeNs == b.mtimeNs && a.ctimeNs == b.ctimeNs; }
inline bool same(const MaintenanceFingerprint& a, const MaintenanceFingerprint& b) { return a.metadata == b.metadata && a.inventory == b.inventory; }
template <class Id>
std::optional<Id> before(Id id) {
    auto s = id.toString();
    for (auto i = s.size(); i--;) {
        if (s[i] == '-') continue;
        if (s[i] == '0') {
            s[i] = 'f';
            continue;
        }
        s[i] = s[i] == 'a' ? '9' : static_cast<char>(s[i] - 1);
        break;
    }
    auto value = Id::parse(s);
    return value ? std::optional<Id>{ value.value() } : std::nullopt;
}
template <class T>
bool less(const T& a, const T& b) { return a < b; }
template <class Tag, std::int64_t Min, std::int64_t Max>
bool less(Quantity<Tag, Min, Max> a, Quantity<Tag, Min, Max> b) { return a.value() < b.value(); }
template <class Id, class Fetch, class Visit>
void pages(const MaintenanceContext& c, Fetch fetch, Visit visit) {
    std::optional<Id> cursor;
    std::size_t count = 0;
    do {
        check(c);
        auto page = take(fetch(cursor));
        if (page.items.size() > 200 || (page.next && cursor && !less(*cursor, *page.next))) throw failure(c, MaintenanceError::CheckIncomplete);
        for (const auto& row : page.items) {
            if (++count > 100000) throw failure(c, MaintenanceError::CheckIncomplete);
            visit(row);
        }
        cursor = page.next;
    } while (cursor);
}
}  // namespace anof::maintenance
namespace anof::maintenance {
MaintenanceFingerprint continuation(const MaintenanceContext&, MetadataInspectionReader&, MaintenanceFileStore&,
                                    const InstanceLease&, MaintenanceDigest&, const MaintenancePlan&, const Sha256& configuration);
}
