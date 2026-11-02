#include <apps/anof/MaintenanceOutput.h>
#include <apps/anof/MaintenancePlanJson.h>

#include <charconv>
#include <set>
#include <stdexcept>
namespace anof::app {
namespace {
using J = nlohmann::json;
[[noreturn]] void invalid() { throw std::invalid_argument("invalid plan"); }
void keys(const J& j, std::initializer_list<const char*> names) {
    if (!j.is_object() || j.size() != names.size()) invalid();
    for (auto name : names)
        if (!j.contains(name)) invalid();
}
template <class T>
T number(const J& j) {
    if (!j.is_string()) invalid();
    const auto& s = j.get_ref<const std::string&>();
    T n{};
    auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || std::to_string(n) != s) invalid();
    return n;
}
template <class Q>
Q quantity(const J& j) {
    auto v = Q::from(number<std::int64_t>(j));
    if (!v) invalid();
    return v.value();
}
template <class I>
I id(const J& j) {
    if (!j.is_string()) invalid();
    auto v = I::parse(j.get_ref<const std::string&>());
    if (!v || v.value().toString() != j.get_ref<const std::string&>()) invalid();
    return v.value();
}
Sha256 hash(const J& j) {
    if (!j.is_string()) invalid();
    auto v = Sha256::parse(j.get_ref<const std::string&>());
    if (!v || v.value().toString() != j.get_ref<const std::string&>()) invalid();
    return v.value();
}
bool boolean(const J& j) {
    if (!j.is_boolean()) invalid();
    return j.get<bool>();
}
int version(const J& j) {
    if (!j.is_number_integer() || j != 5) invalid();
    return 5;
}
J file(const FileEvidence& v) { return { { "device", std::to_string(v.device) }, { "inode", std::to_string(v.inode) }, { "size", std::to_string(v.size.value()) }, { "mtime_ns", std::to_string(v.mtimeNs) }, { "ctime_ns", std::to_string(v.ctimeNs) } }; }
FileEvidence file(const J& j) {
    keys(j, { "device", "inode", "size", "mtime_ns", "ctime_ns" });
    return { number<std::uint64_t>(j.at("device")), number<std::uint64_t>(j.at("inode")), quantity<ByteCount>(j.at("size")), number<std::int64_t>(j.at("mtime_ns")), number<std::int64_t>(j.at("ctime_ns")) };
}
J fingerprint(const MaintenanceFingerprint& f) { return { { "metadata", f.metadata.toString() }, { "inventory", f.inventory.toString() } }; }
MaintenanceFingerprint fingerprint(const J& j) {
    keys(j, { "metadata", "inventory" });
    return { hash(j.at("metadata")), hash(j.at("inventory")) };
}
J report(const MaintenanceReport& r) { return { { "info", std::to_string(r.info.value()) }, { "warnings", std::to_string(r.warnings.value()) }, { "errors", std::to_string(r.errors.value()) }, { "complete", r.complete }, { "details_truncated", r.detailsTruncated } }; }
MaintenanceReport report(const J& j) {
    keys(j, { "info", "warnings", "errors", "complete", "details_truncated" });
    return { quantity<ByteCount>(j.at("info")), quantity<ByteCount>(j.at("warnings")), quantity<ByteCount>(j.at("errors")), boolean(j.at("complete")), boolean(j.at("details_truncated")) };
}
J payload(const MaintenanceActionPayload& p) {
    return std::visit([](const auto& v) -> J {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, RecomputeAccounting>)
            return { { "type", "recompute_accounting" }, { "proven_ledger", v.provenLedger.toString() } };
        else if constexpr (std::is_same_v<T, TruncateUnconfirmedTail>)
            return { { "type", "truncate_unconfirmed_tail" }, { "id", v.id.toString() }, { "generation", std::to_string(v.generation.value()) }, { "confirmed", std::to_string(v.confirmed.value()) }, { "before", file(v.before) } };
        else if constexpr (std::is_same_v<T, ResumeUpload>)
            return { { "type", "resume_upload" }, { "id", v.id.toString() }, { "generation", std::to_string(v.generation.value()) }, { "responsibility", v.responsibility.toString() } };
        else if constexpr (std::is_same_v<T, ResumeDeletingBlob>)
            return { { "type", "resume_deleting_blob" }, { "id", v.id.toString() }, { "revision", std::to_string(v.revision.value()) }, { "responsibility", v.responsibility.toString() } };
        else if constexpr (std::is_same_v<T, QuarantineOrphan>)
            return { { "type", "quarantine_orphan" }, { "source_kind", v.source.index() == 0 ? "blob" : "upload" }, { "source", std::visit([](auto x) { return x.toString(); }, v.source) }, { "destination", v.destination.toString() }, { "before", file(v.before) }, { "sha256", v.sha256.toString() } };
        else if constexpr (std::is_same_v<T, MarkBlobUnavailable>)
            return { { "type", "mark_blob_unavailable" }, { "id", v.id.toString() }, { "revision", std::to_string(v.revision.value()) }, { "reason", v.evidence.reason == UnavailableReason::Missing ? "missing" : v.evidence.reason == UnavailableReason::Length ? "length"
                                                                                                                                                                                                                                                                  : "hash" },
                     { "file", v.evidence.file ? file(*v.evidence.file) : J(nullptr) },
                     { "sha256", v.evidence.sha256 ? J(v.evidence.sha256->toString()) : J(nullptr) } };
        else
            return { { "type", "verify_restored_blob" }, { "id", v.id.toString() }, { "revision", std::to_string(v.revision.value()) }, { "observed", file(v.observed) }, { "sha256", v.sha256.toString() } };
    },
                      p);
}
MaintenanceActionPayload payload(const J& j) {
    auto type = j.at("type").get<std::string>();
    if (type == "recompute_accounting") {
        keys(j, { "type", "proven_ledger" });
        return RecomputeAccounting{ hash(j.at("proven_ledger")) };
    }
    if (type == "truncate_unconfirmed_tail") {
        keys(j, { "type", "id", "generation", "confirmed", "before" });
        return TruncateUnconfirmedTail{ id<UploadId>(j.at("id")), quantity<UploadGeneration>(j.at("generation")), quantity<UploadOffset>(j.at("confirmed")), file(j.at("before")) };
    }
    if (type == "resume_upload") {
        keys(j, { "type", "id", "generation", "responsibility" });
        return ResumeUpload{ id<UploadId>(j.at("id")), quantity<UploadGeneration>(j.at("generation")), hash(j.at("responsibility")) };
    }
    if (type == "resume_deleting_blob") {
        keys(j, { "type", "id", "revision", "responsibility" });
        return ResumeDeletingBlob{ id<BlobId>(j.at("id")), quantity<Revision>(j.at("revision")), hash(j.at("responsibility")) };
    }
    if (type == "quarantine_orphan") {
        keys(j, { "type", "source_kind", "source", "destination", "before", "sha256" });
        std::variant<BlobId, UploadId> source = id<BlobId>(j.at("source"));
        if (j.at("source_kind") == "upload")
            source = id<UploadId>(j.at("source"));
        else if (j.at("source_kind") != "blob")
            invalid();
        return QuarantineOrphan{ source, id<QuarantineId>(j.at("destination")), file(j.at("before")), hash(j.at("sha256")) };
    }
    if (type == "mark_blob_unavailable") {
        keys(j, { "type", "id", "revision", "reason", "file", "sha256" });
        UnavailableReason reason;
        if (j.at("reason") == "missing")
            reason = UnavailableReason::Missing;
        else if (j.at("reason") == "length")
            reason = UnavailableReason::Length;
        else if (j.at("reason") == "hash")
            reason = UnavailableReason::Hash;
        else
            invalid();
        return MarkBlobUnavailable{ id<BlobId>(j.at("id")), quantity<Revision>(j.at("revision")), { reason, j.at("file").is_null() ? std::optional<FileEvidence>{} : file(j.at("file")), j.at("sha256").is_null() ? std::optional<Sha256>{} : hash(j.at("sha256")) } };
    }
    if (type == "verify_restored_blob") {
        keys(j, { "type", "id", "revision", "observed", "sha256" });
        return VerifyRestoredBlob{ id<BlobId>(j.at("id")), quantity<Revision>(j.at("revision")), file(j.at("observed")), hash(j.at("sha256")) };
    }
    invalid();
}
}  // namespace
nlohmann::json maintenancePlanData(const MaintenancePlan& p) {
    J actions = J::array();
    for (const auto& a : p.actions) actions.push_back({ { "id", a.id.toString() }, { "payload", payload(a.payload) }, { "expected_postcondition", a.expectedPostcondition.toString() } });
    return { { "plan_format", p.planFormat }, { "id", p.id.toString() }, { "instance_id", p.instance.toString() }, { "schema", p.schemaVersion }, { "format", p.formatVersion }, { "source", fingerprint(p.source) }, { "continuation", p.continuation ? fingerprint(*p.continuation) : J(nullptr) }, { "mode", p.mode == InspectionMode::Full ? "full" : "quick" }, { "inspection", report(p.inspection) }, { "actions", std::move(actions) } };
}
Result<MaintenancePlan> parseMaintenancePlan(std::string_view bytes) {
    try {
        if (bytes.size() > kMaintenancePlanBytes) invalid();
        std::vector<std::set<std::string>> names;
        auto j = J::parse(bytes, [&](int depth, J::parse_event_t event, J& value) {
            if (depth > static_cast<int>(kMaintenanceJsonDepth)) invalid();
            if (event == J::parse_event_t::object_start) names.emplace_back();
            if (event == J::parse_event_t::key && (names.empty() || !names.back().insert(value.get<std::string>()).second)) invalid();
            if (event == J::parse_event_t::object_end) names.pop_back();
            return true;
        });
        keys(j, { "plan_format", "id", "instance_id", "schema", "format", "source", "continuation", "mode", "inspection", "actions" });
        if (!j.at("plan_format").is_number_integer() || j.at("plan_format") != 1) invalid();
        if (j.at("mode") != "full" && j.at("mode") != "quick") invalid();
        MaintenancePlan p{ id<MaintenancePlanId>(j.at("id")), id<InstanceId>(j.at("instance_id")), version(j.at("schema")), version(j.at("format")), 1, fingerprint(j.at("source")), j.at("mode") == "full" ? InspectionMode::Full : InspectionMode::Quick, report(j.at("inspection")), {}, fingerprint(j.at("continuation")) };
        const auto& a = j.at("actions");
        if (!a.is_array() || a.size() > kMaintenanceActionLimit) invalid();
        for (const auto& v : a) {
            keys(v, { "id", "payload", "expected_postcondition" });
            p.actions.push_back({ id<MaintenanceActionId>(v.at("id")), payload(v.at("payload")), hash(v.at("expected_postcondition")) });
        }
        if (!validateMaintenancePlan(p)) invalid();
        return p;
    } catch (...) {
        return ErrorCode::InvalidArgument;
    }
}
nlohmann::json maintenanceFindingData(const MaintenanceFinding& f) {
    static constexpr const char* codes[] = { "DATABASE_CORRUPT", "FOREIGN_KEY_BROKEN", "TREE_INVALID", "ACCOUNTING_MISMATCH", "UPLOAD_STATE_INCONSISTENT", "OBJECT_MISSING", "OBJECT_LENGTH_MISMATCH", "OBJECT_HASH_MISMATCH", "ORPHAN_OBJECT", "UNSAFE_ENTRY", "QUARANTINE_PRESENT", "MAINTENANCE_PENDING", "CHECK_LIMIT_EXCEEDED" };
    static constexpr const char* resources[] = { "instance", "database", "user", "space", "node", "blob", "upload", "quarantine", "run" };
    return { { "type", "finding" }, { "code", codes[static_cast<std::size_t>(f.code)] }, { "severity", f.severity == FindingSeverity::Error ? "error" : f.severity == FindingSeverity::Warning ? "warning"
                                                                                                                                                                                               : "info" },
             { "resource_type", resources[static_cast<std::size_t>(f.resource)] },
             { "resource_id", f.resourceId ? J(f.resourceId->toString()) : J(nullptr) },
             { "relative_key", f.file ? J(maintenanceStorageKey(*f.file)) : J(nullptr) },
             { "repairable", f.repairable } };
}
nlohmann::json maintenanceSummaryData(const MaintenanceReport& r) {
    auto j = report(r);
    j["type"] = "summary";
    j["status"] = maintenanceStatusName(r.status());
    j["source_changed"] = false;
    return j;
}
}  // namespace anof::app
