#include <anof/application/maintenance/Fsck.h>

#include <type_traits>
namespace anof {
namespace {
struct Encode {
    std::string bytes;
    void put(std::string_view s) {
        bytes += std::to_string(s.size()) + ":";
        bytes.append(s);
    }
    void put(std::int64_t n) { put(std::to_string(n)); }
    void put(const FileEvidence& f) {
        put(std::to_string(f.device));
        put(std::to_string(f.inode));
        put(f.size.value());
        put(f.mtimeNs);
        put(f.ctimeNs);
    }
};
}  // namespace
std::string maintenanceActionBytes(const MaintenanceActionPayload& value) {
    Encode e;
    e.put("ANOF-ACTION-1");
    e.put(static_cast<std::int64_t>(value.index()));
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, RecomputeAccounting>)
            e.put(p.provenLedger.toString());
        else if constexpr (std::is_same_v<T, QuarantineOrphan>) {
            e.put(static_cast<std::int64_t>(p.source.index()));
            std::visit([&](const auto& id) { e.put(id.toString()); }, p.source);
            e.put(p.destination.toString());
            e.put(p.before);
            e.put(p.sha256.toString());
        } else {
            e.put(p.id.toString());
            if constexpr (std::is_same_v<T, TruncateUnconfirmedTail>) {
                e.put(p.generation.value());
                e.put(p.confirmed.value());
                e.put(p.before);
            } else if constexpr (std::is_same_v<T, ResumeUpload>) {
                e.put(p.generation.value());
                e.put(p.responsibility.toString());
            } else {
                e.put(p.revision.value());
                if constexpr (std::is_same_v<T, ResumeDeletingBlob>)
                    e.put(p.responsibility.toString());
                else if constexpr (std::is_same_v<T, VerifyRestoredBlob>) {
                    e.put(p.observed);
                    e.put(p.sha256.toString());
                } else {
                    e.put(static_cast<std::int64_t>(p.evidence.reason));
                    e.put(p.evidence.file ? 1 : 0);
                    if (p.evidence.file) e.put(*p.evidence.file);
                    e.put(p.evidence.sha256 ? p.evidence.sha256->toString() : "");
                }
            }
        }
    },
               value);
    return e.bytes;
}
std::string maintenancePlanBytes(const MaintenancePlan& p) {
    Encode e;
    e.put("ANOF-PLAN-1");
    e.put(p.id.toString());
    e.put(p.instance.toString());
    e.put(p.schemaVersion);
    e.put(p.formatVersion);
    e.put(p.planFormat);
    e.put(p.source.metadata.toString());
    e.put(p.source.inventory.toString());
    e.put(static_cast<std::int64_t>(p.mode));
    e.put(p.inspection.info.value());
    e.put(p.inspection.warnings.value());
    e.put(p.inspection.errors.value());
    e.put(p.inspection.complete);
    e.put(p.inspection.detailsTruncated);
    e.put(p.continuation ? 1 : 0);
    if (p.continuation) {
        e.put(p.continuation->metadata.toString());
        e.put(p.continuation->inventory.toString());
    }
    e.put(static_cast<std::int64_t>(p.actions.size()));
    for (const auto& a : p.actions) {
        e.put(a.id.toString());
        e.put(maintenanceActionBytes(a.payload));
        e.put(a.expectedPostcondition.toString());
    }
    return e.bytes;
}
}  // namespace anof
