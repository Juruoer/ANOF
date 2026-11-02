#include <anof/application/maintenance/internal/FsckSupport.h>

#include <map>
#include <set>
namespace anof::maintenance {
MaintenanceFingerprint continuation(const MaintenanceContext& c, MetadataInspectionReader& reader, MaintenanceFileStore& files,
                                    const InstanceLease& lease, MaintenanceDigest& digest, const MaintenancePlan& plan, const Sha256& configuration) {
    auto metadata = checked(c, digest.digest(take(reader.repairInvariant(c, plan)).toString() + configuration.toString()));
    std::set<MaintenanceFileKey> allowed;
    for (const auto& a : plan.actions) std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, QuarantineOrphan>) {
            allowed.insert(std::visit([](auto id) -> MaintenanceFileKey { return id; }, p.source));
            allowed.insert(p.destination);
        } else if constexpr (!std::is_same_v<T, RecomputeAccounting>) {
            allowed.insert(p.id);
            if constexpr (std::is_same_v<T, ResumeUpload>) {
                auto page = take(reader.uploads(c, PageSize::from(1).value(), before(p.id)));
                if (!page.items.empty() && page.items.front().id == p.id && page.items.front().pendingBlobId) allowed.insert(*page.items.front().pendingBlobId);
            }
        }
    },
                                                  a.payload);
    auto hash = checked(c, digest.digest("ANOF-UNCHANGED-FILES-1"));
    std::string after;
    // 目录适配无顺序保证；每遍只保留下一页200项，避免全实例内存索引。
    for (;;) {
        std::map<std::string, MaintenanceFileObservation> selected;
        auto scan = take(files.inventory(c, lease, InspectionMode::Quick));
        std::size_t count = 0;
        for (;;) {
            check(c);
            auto page = take(scan->next(c, PageSize::from(200).value()));
            if (page.empty()) break;
            for (const auto& item : page) {
                if (++count > 100000) throw failure(c, MaintenanceError::CheckIncomplete);
                auto* file = std::get_if<MaintenanceFileObservation>(&item);
                if (!file) throw failure(c, MaintenanceError::UnsafePath);
                auto key = maintenanceStorageKey(file->key);
                if (allowed.count(file->key) || key <= after) continue;
                selected.emplace(key, *file);
                if (selected.size() > 200) selected.erase(std::prev(selected.end()));
            }
        }
        if (selected.empty()) break;
        for (const auto& pair : selected) {
            const auto& f = pair.second.evidence;
            auto bytes = hash.toString() + ":" + pair.first + ":" + std::to_string(f.device) + ":" + std::to_string(f.inode) + ":" + std::to_string(f.size.value()) + ":" + std::to_string(f.mtimeNs) + ":" + std::to_string(f.ctimeNs);
            hash = checked(c, digest.digest(bytes));
            after = pair.first;
        }
    }
    return { metadata, hash };
}
}  // namespace anof::maintenance
