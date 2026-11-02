#include <anof/infrastructure/persistence/sqlite/internal/MaintenanceCodec.h>

#include <stdexcept>
namespace anof::infrastructure::sqlite::maintenance {
namespace {
template <class T>
T id(const Json& j) {
    auto v = T::parse(j.get<std::string>());
    if (!v) throw std::invalid_argument("invalid id");
    return v.value();
}
template <class T>
T quantity(const Json& j) {
    if (!j.is_number_integer() || (j.is_number_unsigned() && j.get<std::uint64_t>() > INT64_MAX)) throw std::invalid_argument("invalid quantity");
    auto v = T::from(j.get<std::int64_t>());
    if (!v) throw std::invalid_argument("invalid quantity");
    return v.value();
}
Sha256 hash(const Json& j) {
    auto v = Sha256::parse(j.get<std::string>());
    if (!v) throw std::invalid_argument("invalid hash");
    return v.value();
}
Json key(const MaintenanceFileKey& k) {
    return { k.index(), std::visit([](const auto& v) { return v.toString(); }, k) };
}
MaintenanceFileKey key(const Json& j) {
    switch (j.at(0).get<int>()) {
    case 0:
        return id<BlobId>(j.at(1));
    case 1:
        return id<UploadId>(j.at(1));
    case 2:
        return id<QuarantineId>(j.at(1));
    default:
        throw std::invalid_argument("invalid key");
    }
}
Json unavailable(const UnavailableEvidence& e) { return { int(e.reason), e.file ? encode(*e.file) : Json(nullptr), e.sha256 ? Json(e.sha256->toString()) : Json(nullptr) }; }
UnavailableEvidence unavailable(const Json& j) { return { static_cast<UnavailableReason>(j.at(0).get<int>()), j.at(1).is_null() ? std::optional<FileEvidence>{} : fileEvidence(j.at(1)), j.at(2).is_null() ? std::optional<Sha256>{} : hash(j.at(2)) }; }
}  // namespace
Json encode(const FileEvidence& e) { return { e.device, e.inode, e.size.value(), e.mtimeNs, e.ctimeNs }; }
FileEvidence fileEvidence(const Json& j) {
    if (!j.is_array() || j.size() != 5) throw std::invalid_argument("invalid evidence");
    for (std::size_t i : { 0u, 1u })
        if (!j.at(i).is_number_integer() || (!j.at(i).is_number_unsigned() && j.at(i).get<std::int64_t>() < 0)) throw std::invalid_argument("invalid identity");
    for (std::size_t i : { 3u, 4u })
        if (!j.at(i).is_number_integer() || (j.at(i).is_number_unsigned() && j.at(i).get<std::uint64_t>() > INT64_MAX)) throw std::invalid_argument("invalid time");
    return { j.at(0).get<std::uint64_t>(), j.at(1).get<std::uint64_t>(), quantity<ByteCount>(j.at(2)), j.at(3).get<std::int64_t>(), j.at(4).get<std::int64_t>() };
}
Json encode(const MaintenanceActionPayload& p) {
    Json j = Json::array();
    j.push_back(p.index());
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, RecomputeAccounting>)
            j.push_back(v.provenLedger.toString());
        else if constexpr (std::is_same_v<T, TruncateUnconfirmedTail>) {
            j.push_back(v.id.toString());
            j.push_back(v.generation.value());
            j.push_back(v.confirmed.value());
            j.push_back(encode(v.before));
        } else if constexpr (std::is_same_v<T, ResumeUpload>) {
            j.push_back(v.id.toString());
            j.push_back(v.generation.value());
            j.push_back(v.responsibility.toString());
        } else if constexpr (std::is_same_v<T, ResumeDeletingBlob>) {
            j.push_back(v.id.toString());
            j.push_back(v.revision.value());
            j.push_back(v.responsibility.toString());
        } else if constexpr (std::is_same_v<T, QuarantineOrphan>) {
            j.push_back(key(std::visit([](const auto& x) -> MaintenanceFileKey { return x; }, v.source)));
            j.push_back(v.destination.toString());
            j.push_back(encode(v.before));
            j.push_back(v.sha256.toString());
        } else if constexpr (std::is_same_v<T, MarkBlobUnavailable>) {
            j.push_back(v.id.toString());
            j.push_back(v.revision.value());
            j.push_back(unavailable(v.evidence));
        } else {
            j.push_back(v.id.toString());
            j.push_back(v.revision.value());
            j.push_back(encode(v.observed));
            j.push_back(v.sha256.toString());
        }
    },
               p);
    return j;
}
MaintenanceActionPayload payload(const Json& j) {
    switch (j.at(0).get<int>()) {
    case 0:
        return RecomputeAccounting{ hash(j.at(1)) };
    case 1:
        return TruncateUnconfirmedTail{ id<UploadId>(j.at(1)), quantity<UploadGeneration>(j.at(2)), quantity<UploadOffset>(j.at(3)), fileEvidence(j.at(4)) };
    case 2:
        return ResumeUpload{ id<UploadId>(j.at(1)), quantity<UploadGeneration>(j.at(2)), hash(j.at(3)) };
    case 3:
        return ResumeDeletingBlob{ id<BlobId>(j.at(1)), quantity<Revision>(j.at(2)), hash(j.at(3)) };
    case 4: {
        auto k = key(j.at(1));
        if (k.index() > 1) throw std::invalid_argument("invalid source");
        std::variant<BlobId, UploadId> src = k.index() == 0 ? std::variant<BlobId, UploadId>{ std::get<BlobId>(k) } : std::variant<BlobId, UploadId>{ std::get<UploadId>(k) };
        return QuarantineOrphan{ src, id<QuarantineId>(j.at(2)), fileEvidence(j.at(3)), hash(j.at(4)) };
    }
    case 5:
        return MarkBlobUnavailable{ id<BlobId>(j.at(1)), quantity<Revision>(j.at(2)), unavailable(j.at(3)) };
    case 6:
        return VerifyRestoredBlob{ id<BlobId>(j.at(1)), quantity<Revision>(j.at(2)), fileEvidence(j.at(3)), hash(j.at(4)) };
    default:
        throw std::invalid_argument("invalid action");
    }
}
Json encode(const MaintenanceActionEvidence& e) {
    Json facts = Json::array();
    for (const auto& f : e.files) facts.push_back({ key(f.key), f.file ? encode(*f.file) : Json(nullptr), f.sha256 ? Json(f.sha256->toString()) : Json(nullptr) });
    const auto& b = e.binding;
    return { { b.instance.toString(), b.run.toString(), b.plan.toString(), b.planDigest.toString(), b.action.toString(), b.actionRevision.value() }, int(e.quality), facts, e.postcondition.toString() };
}
MaintenanceActionEvidence evidence(const Json& j) {
    auto& b = j.at(0);
    MaintenanceActionEvidence e{ { id<InstanceId>(b.at(0)), id<MaintenanceRunId>(b.at(1)), id<MaintenancePlanId>(b.at(2)), hash(b.at(3)), id<MaintenanceActionId>(b.at(4)), quantity<Revision>(b.at(5)) }, static_cast<MaintenanceEvidenceQuality>(j.at(1).get<int>()), {}, hash(j.at(3)) };
    if (j.at(2).size() > 2) throw std::invalid_argument("too many facts");
    for (const auto& f : j.at(2)) e.files.push_back({ key(f.at(0)), f.at(1).is_null() ? std::optional<FileEvidence>{} : fileEvidence(f.at(1)), f.at(2).is_null() ? std::optional<Sha256>{} : hash(f.at(2)) });
    return e;
}
Json encode(const QuarantineEntry& e) {
    return { e.id.toString(), key(std::visit([](const auto& x) -> MaintenanceFileKey { return x; }, e.source)), encode(e.evidence), e.sha256.toString(), e.chargedBytes.value(), int(e.state), e.plan.toString(), e.action.toString(), e.createdAt.time_since_epoch().count(), e.updatedAt.time_since_epoch().count(), e.revision.value() };
}
QuarantineEntry quarantine(const Json& j) {
    auto k = key(j.at(1));
    if (k.index() > 1) throw std::invalid_argument("invalid source");
    std::variant<BlobId, UploadId> src = k.index() == 0 ? std::variant<BlobId, UploadId>{ std::get<BlobId>(k) } : std::variant<BlobId, UploadId>{ std::get<UploadId>(k) };
    return { id<QuarantineId>(j.at(0)), src, fileEvidence(j.at(2)), hash(j.at(3)), quantity<ByteCount>(j.at(4)), static_cast<QuarantineState>(j.at(5).get<int>()), id<MaintenancePlanId>(j.at(6)), id<MaintenanceActionId>(j.at(7)), UtcTime{ std::chrono::microseconds{ j.at(8).get<std::int64_t>() } }, UtcTime{ std::chrono::microseconds{ j.at(9).get<std::int64_t>() } }, quantity<Revision>(j.at(10)) };
}
}  // namespace anof::infrastructure::sqlite::maintenance
