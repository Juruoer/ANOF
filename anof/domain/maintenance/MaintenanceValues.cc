#include <anof/domain/maintenance/MaintenanceValues.h>
#include <set>
#include <type_traits>

namespace anof {
std::string maintenanceStorageKey(const MaintenanceFileKey& key) {
    return std::visit([](const auto& id) {
        auto text = id.toString();
        using T = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<T, BlobId>) return "objects/" + text.substr(0, 2) + "/" + text;
        else if constexpr (std::is_same_v<T, UploadId>) return "uploads/" + text + ".part";
        else return "quarantine/" + text;
    }, key);
}
MaintenanceReportStatus MaintenanceReport::status() const noexcept {
    if (!complete || detailsTruncated) return MaintenanceReportStatus::Incomplete;
    return warnings.value() || errors.value() ? MaintenanceReportStatus::Findings : MaintenanceReportStatus::Healthy;
}
Result<void> validateMaintenancePlan(const MaintenancePlan& p) {
    if (p.schemaVersion != 5 || p.formatVersion != 5 || p.planFormat != kMaintenancePlanFormat ||
        (p.mode != InspectionMode::Quick && p.mode != InspectionMode::Full) ||
        p.inspection.status() == MaintenanceReportStatus::Incomplete || p.actions.size() > kMaintenanceActionLimit)
        return ErrorCode::InvalidArgument;
    std::set<MaintenanceActionId> ids;
    std::set<QuarantineId> destinations;
    for (const auto& action : p.actions) {
        if (!ids.insert(action.id).second) return ErrorCode::InvalidArgument;
        if (const auto* tail = std::get_if<TruncateUnconfirmedTail>(&action.payload)) {
            if (tail->confirmed.value() > kMaxFileBytes || tail->confirmed.value() >= tail->before.size.value())
                return ErrorCode::InvalidArgument;
        }
        if (const auto* orphan = std::get_if<QuarantineOrphan>(&action.payload))
            if (!destinations.insert(orphan->destination).second) return ErrorCode::InvalidArgument;
        if (std::holds_alternative<VerifyRestoredBlob>(action.payload) && p.mode != InspectionMode::Full)
            return ErrorCode::InvalidArgument;
    }
    // 这里只验证自包含结构；源绑定、证据及合法动作重建必须由执行入口在持锁后复验。
    return {};
}
Result<void> validateQuarantineEntry(const QuarantineEntry& e) {
    if (e.chargedBytes != e.evidence.size || e.updatedAt < e.createdAt ||
        (e.state != QuarantineState::Prepared && e.state != QuarantineState::Held)) return ErrorCode::IntegrityError;
    return {};
}
Result<void> validateMaintenanceEvidence(const MaintenanceEvidenceBinding& b, const MaintenanceAction& a,
    const MaintenanceActionEvidence& e) {
    const auto& actual = e.binding;
    if (actual.instance != b.instance || actual.run != b.run || actual.plan != b.plan ||
        actual.planDigest != b.planDigest || actual.action != b.action || b.action != a.id ||
        actual.actionRevision != b.actionRevision || e.quality != MaintenanceEvidenceQuality::Durable ||
        e.postcondition != a.expectedPostcondition || e.files.size() > 2) return ErrorCode::IntegrityError;
    std::set<MaintenanceFileKey> keys;
    for (const auto& f : e.files)
        if (!keys.insert(f.key).second || (!f.file && f.sha256)) return ErrorCode::IntegrityError;
    if (std::holds_alternative<RecomputeAccounting>(a.payload)) {
        if (!e.files.empty()) return ErrorCode::IntegrityError;
    } else if (e.files.empty()) return ErrorCode::IntegrityError;
    auto find = [&](const MaintenanceFileKey& key) -> const MaintenanceFileFact* {
        for (const auto& f : e.files) if (f.key == key) return &f;
        return nullptr;
    };
    auto sameFile = [](const FileEvidence& x, const FileEvidence& y) {
        return x.device == y.device && x.inode == y.inode && x.size == y.size &&
            x.mtimeNs == y.mtimeNs && x.ctimeNs == y.ctimeNs;
    };
    if (const auto* tail = std::get_if<TruncateUnconfirmedTail>(&a.payload)) {
        const auto* f = find(tail->id);
        if (e.files.size() != 1 || !f || !f->file || f->file->size.value() != tail->confirmed.value() ||
            f->file->device != tail->before.device || f->file->inode != tail->before.inode) return ErrorCode::IntegrityError;
    } else if (const auto* q = std::get_if<QuarantineOrphan>(&a.payload)) {
        auto source = std::visit([](const auto& id) -> MaintenanceFileKey { return id; }, q->source);
        const auto* src = find(source); const auto* dst = find(q->destination);
        if (e.files.size() != 2 || !src || src->file || !dst || !dst->file || !dst->sha256 ||
            *dst->sha256 != q->sha256 || dst->file->size != q->before.size ||
            dst->file->device != q->before.device || dst->file->inode != q->before.inode) return ErrorCode::IntegrityError;
    } else if (const auto* restored = std::get_if<VerifyRestoredBlob>(&a.payload)) {
        const auto* f = find(restored->id);
        if (e.files.size() != 1 || !f || !f->file || !f->sha256 || *f->sha256 != restored->sha256 ||
            !sameFile(*f->file, restored->observed)) return ErrorCode::IntegrityError;
    } else if (const auto* marked = std::get_if<MarkBlobUnavailable>(&a.payload)) {
        const auto* f = find(marked->id);
        if (e.files.size() != 1 || !f || bool(f->file) != bool(marked->evidence.file) ||
            f->sha256 != marked->evidence.sha256 ||
            (f->file && !sameFile(*f->file, *marked->evidence.file))) return ErrorCode::IntegrityError;
    } else if (const auto* deleted = std::get_if<ResumeDeletingBlob>(&a.payload)) {
        const auto* f = find(deleted->id);
        if (e.files.size() != 1 || !f || f->file) return ErrorCode::IntegrityError;
    }
    // ResumeUpload的具体位置/摘要还需与持久Upload状态、generation和pendingBlobId核对。
    return {};
}
Result<void> validateMaintenanceActionRecord(const MaintenanceActionRecord& r) {
    if (r.updatedAt < r.createdAt ||
        (r.state != MaintenanceActionState::Prepared && r.state != MaintenanceActionState::Applied &&
         r.state != MaintenanceActionState::Verified)) return ErrorCode::IntegrityError;
    if (r.state == MaintenanceActionState::Prepared) {
        if (r.resultEvidence) return ErrorCode::IntegrityError;
    } else {
        if (!r.resultEvidence || !validateMaintenanceEvidence(r.resultEvidence->binding, r.action, *r.resultEvidence))
            return ErrorCode::IntegrityError;
        auto next = r.resultEvidence->binding.actionRevision.add(1);
        if (!next || next.value() != r.revision) return ErrorCode::IntegrityError;
    }
    return {};
}
Result<void> validateBackupManifest(const BackupManifest& m) {
    if (m.backupFormat != kBackupFormat || m.backend != "sqlite" || m.schemaVersion != 5 || m.formatVersion != 5 ||
        m.applicationVersion.empty() || m.applicationVersion.size() > 128 || m.database.size.value() == 0 ||
        m.configuration.size.value() == 0 || m.configuration.size.value() > 65536 ||
        m.objects.count.value() > static_cast<std::int64_t>(kBackupObjectLimit) ||
        m.objects.file.size.value() > static_cast<std::int64_t>(kBackupObjectLineBytes * kBackupObjectLimit))
        return ErrorCode::InvalidArgument;
    for (unsigned char c : m.applicationVersion) if (c < 0x21 || c > 0x7e) return ErrorCode::InvalidArgument;
    if ((m.objects.count.value() == 0 && (m.objects.totalBytes.value() || m.objects.file.size.value())) ||
        (m.objects.count.value() != 0 && m.objects.file.size.value() == 0)) return ErrorCode::InvalidArgument;
    return {};
}
Result<void> BackupIndexValidator::append(const Blob& b) {
    if ((last_ && !(last_.value() < b.id)) || count_.value() >= static_cast<std::int64_t>(kBackupObjectLimit))
        return ErrorCode::IntegrityError;
    auto sum = bytes_.add(b.size.value());
    if (!sum) return sum.error();
    bytes_ = sum.value(); count_ = count_.add(1).value(); last_ = b.id;
    return {};
}
Result<void> BackupIndexValidator::finish(const BackupObjectIndex& index) const {
    if (count_ != index.count || bytes_ != index.totalBytes) return ErrorCode::IntegrityError;
    return {};
}
}  // namespace anof
