#define BOOST_TEST_MODULE MaintenanceValues
#include <anof/application/maintenance/PrepareInstance.h>
#include <anof/application/ports/MaintenanceResult.h>
#include <anof/domain/drive/NodeAccounting.h>
#include <anof/domain/maintenance/MaintenanceValues.h>
#include <anof/domain/transfer/Storage.h>

#include <boost/test/unit_test.hpp>

using namespace anof;
namespace {
template <class T>
T id(char c = '1') { return T::parse(std::string(8, c) + "-1111-1111-1111-111111111111").value(); }
ByteCount bytes(std::int64_t n) { return ByteCount::from(n).value(); }
Sha256 hash(char c = 'a') { return Sha256::parse(std::string(64, c)).value(); }
Blob blob(char c = '1') { return { id<BlobId>(c), FileLength::from(3).value(), hash() }; }
MaintenancePlan plan() { return { id<MaintenancePlanId>(), id<InstanceId>(), 5, 5, 1, { hash(), hash() }, InspectionMode::Full, {}, { { id<MaintenanceActionId>(), RecomputeAccounting{ hash() }, hash() } } }; }
}  // namespace
BOOST_AUTO_TEST_CASE(report_and_plan_never_hide_incomplete_checks) {
    MaintenanceReport r;
    BOOST_CHECK(r.status() == MaintenanceReportStatus::Healthy);
    r.info = bytes(1);
    BOOST_CHECK(r.status() == MaintenanceReportStatus::Healthy);
    r.warnings = bytes(1);
    BOOST_CHECK(r.status() == MaintenanceReportStatus::Findings);
    r.complete = false;
    BOOST_CHECK(r.status() == MaintenanceReportStatus::Incomplete);
    auto p = plan();
    BOOST_REQUIRE(validateMaintenancePlan(p));
    p.inspection = r;
    BOOST_CHECK(!validateMaintenancePlan(p));
    p = plan();
    p.inspection.detailsTruncated = true;
    BOOST_CHECK(!validateMaintenancePlan(p));
    p = plan();
    p.actions.push_back(p.actions.front());
    BOOST_CHECK(!validateMaintenancePlan(p));
    p = plan();
    p.schemaVersion = 4;
    BOOST_CHECK(!validateMaintenancePlan(p));
    p = plan();
    p.actions.resize(kMaintenanceActionLimit + 1, p.actions.front());
    BOOST_CHECK(!validateMaintenancePlan(p));
}
BOOST_AUTO_TEST_CASE(unavailable_retains_charge_and_is_never_gc_eligible) {
    BlobRecord r{ blob(), BlobState::Unavailable, UtcTime{}, UtcTime{}, Revision{}, UtcTime{}, {}, bytes(3), UnavailableEvidence{ UnavailableReason::Missing, {}, {} } };
    BOOST_REQUIRE(validateBlobRecord(r));
    BOOST_CHECK(!blobGcEligible(r, UtcTime{} + std::chrono::hours(100), false, false));
    r.chargedBytes = bytes(0);
    BOOST_CHECK(!validateBlobRecord(r));
    r.unavailable = UnavailableEvidence{ UnavailableReason::Length, FileEvidence{ 1, 2, bytes(9), 3, 4 }, {} };
    r.chargedBytes = bytes(9);
    BOOST_REQUIRE(validateBlobRecord(r));
    r.unavailable->sha256 = hash('b');
    BOOST_CHECK(!validateBlobRecord(r));
    r.unavailable = UnavailableEvidence{ UnavailableReason::Hash, FileEvidence{ 1, 2, bytes(3), 3, 4 }, hash('b') };
    r.chargedBytes = bytes(3);
    BOOST_REQUIRE(validateBlobRecord(r));
    r.unavailable->sha256 = hash();
    BOOST_CHECK(!validateBlobRecord(r));
    r.state = BlobState::Available;
    BOOST_CHECK(!validateBlobRecord(r));
    r.unavailable.reset();
    BOOST_REQUIRE(validateBlobRecord(r));
}
BOOST_AUTO_TEST_CASE(excess_physical_charge_allows_release_not_new_reservations) {
    StorageSnapshot s{ bytes(5), bytes(8), bytes(2), {} };
    BOOST_CHECK(!reserveStorage(s, bytes(0)));
    BOOST_CHECK(!setStorageBudget(s, s.revision, bytes(9)));
    BOOST_REQUIRE(releaseRetainedStorage(s, bytes(1)));
    BOOST_REQUIRE(releaseStorage(s, bytes(1)));
    BOOST_REQUIRE(setStorageBudget(s, s.revision, bytes(10)));
    s.retained = bytes(INT64_MAX);
    BOOST_CHECK(!releaseStorage(s, bytes(1)));
}
BOOST_AUTO_TEST_CASE(canonical_keys_and_index_are_bounded_and_ordered) {
    BOOST_CHECK_EQUAL(maintenanceStorageKey(id<BlobId>()), blob().storageKey());
    BOOST_CHECK_EQUAL(maintenanceStorageKey(id<UploadId>()), "uploads/" + id<UploadId>().toString() + ".part");
    BOOST_CHECK_EQUAL(maintenanceStorageKey(id<QuarantineId>()), "quarantine/" + id<QuarantineId>().toString());
    BackupIndexValidator index;
    BOOST_REQUIRE(index.append(blob()));
    BOOST_CHECK(!index.append(blob()));
    BOOST_REQUIRE(index.append(blob('2')));
    BackupObjectIndex expected{ { bytes(200), hash() }, NodeCount::from(2).value(), bytes(6) };
    BOOST_REQUIRE(index.finish(expected));
    expected.totalBytes = bytes(3);
    BOOST_CHECK(!index.finish(expected));
}
BOOST_AUTO_TEST_CASE(action_and_quarantine_require_durable_evidence) {
    auto p = plan();
    MaintenanceActionRecord r{ {}, p.actions.front(), MaintenanceActionState::Verified, {}, UtcTime{}, UtcTime{}, {} };
    BOOST_CHECK(!validateMaintenanceActionRecord(r));
    r.resultEvidence = MaintenanceActionEvidence{ { id<InstanceId>(), id<MaintenanceRunId>(), id<MaintenancePlanId>(), hash(),
                                                    p.actions.front().id, Revision{} },
                                                  MaintenanceEvidenceQuality::Durable,
                                                  {},
                                                  hash() };
    r.revision = Revision::from(2).value();
    BOOST_REQUIRE(validateMaintenanceActionRecord(r));
    r.resultEvidence->postcondition = hash('b');
    BOOST_CHECK(!validateMaintenanceActionRecord(r));
    QuarantineEntry q{ id<QuarantineId>(), id<BlobId>(), { 1, 2, bytes(7), 3, 4 }, hash(), bytes(7), QuarantineState::Prepared, id<MaintenancePlanId>(), id<MaintenanceActionId>(), UtcTime{}, UtcTime{}, {} };
    BOOST_REQUIRE(validateQuarantineEntry(q));
    q.chargedBytes = bytes(0);
    BOOST_CHECK(!validateQuarantineEntry(q));
}
BOOST_AUTO_TEST_CASE(origin_normalization_and_dangerous_configuration_combinations) {
    for (auto input : { "https://Example.COM:443", "https://example.com" }) {
        auto parsed = PublicOrigin::parse(input);
        BOOST_REQUIRE(parsed);
        BOOST_CHECK_EQUAL(parsed.value().value(), "https://example.com");
    }
    BOOST_REQUIRE(PublicOrigin::parse("https://[::1]:8443"));
    for (auto input : { "null", "http://example.com", "https://x/", "https://u@x", "https://x?q", "https://x#q",
                        "https://x:0", "https://x:65536", "https://x:0443", "https://x:", "https://127.1", "https://a..b", "https://[::1%eth0]", "https://0x7f000001", "https://example.123" })
        BOOST_CHECK_MESSAGE(!PublicOrigin::parse(input), input);
    InstanceConfiguration c;
    c.developmentHttp = true;
    BOOST_REQUIRE(validateInstanceConfiguration(c));
    c.webEnabled = true;
    BOOST_CHECK(!validateInstanceConfiguration(c));
    c.developmentHttp = false;
    c.publicOrigin = "https://example.com";
    c.trustedProxies = { "127.0.0.1", "::1" };
    BOOST_REQUIRE(validateInstanceConfiguration(c));
    c.trustedProxies = { "127.0.0.1", "127.0.0.1" };
    BOOST_CHECK(!validateInstanceConfiguration(c));
    c.trustedProxies = { "127.0.0.0/8" };
    BOOST_CHECK(!validateInstanceConfiguration(c));
    c.trustedProxies = { "127.0.0.1" };
    c.publicOrigin = "https://EXAMPLE.com";
    BOOST_CHECK(!validateInstanceConfiguration(c));
    BOOST_CHECK(!AuditRetentionDays::from(6));
    BOOST_CHECK(!AuditRetentionDays::from(3651));
    BOOST_CHECK(!AuditBatchSize::from(0));
    BOOST_CHECK(!AuditBatchSize::from(1001));
}
BOOST_AUTO_TEST_CASE(storage_subsets_are_disjoint_checked_counts_not_extra_charge) {
    GcStatistics s{ bytes(2), NodeCount::from(1).value(), {}, {}, bytes(3), NodeCount::from(1).value(), bytes(4), NodeCount::from(1).value() };
    BOOST_REQUIRE(validateStorageStatistics(s, bytes(9)));
    BOOST_CHECK(!validateStorageStatistics(s, bytes(8)));
    s.deletingBytes = bytes(3);
    BOOST_CHECK(!validateStorageStatistics(s, bytes(9)));
    s.deletingBytes = {};
    s.quarantinedBytes = bytes(INT64_MAX);
    BOOST_CHECK(!validateStorageStatistics(s, bytes(INT64_MAX)));
}
BOOST_AUTO_TEST_CASE(backup_manifest_rejects_old_formats_and_unbounded_index) {
    BackupManifest m{ 1, "sqlite", "0.1.0", 5, 5, id<BackupId>(), id<InstanceId>(), UtcTime{}, { bytes(4096), hash() }, { bytes(100), hash() }, { { bytes(0), hash() }, {}, {} }, {} };
    BOOST_REQUIRE(validateBackupManifest(m));
    m.schemaVersion = 4;
    BOOST_CHECK(!validateBackupManifest(m));
    m.schemaVersion = 5;
    m.configuration.size = bytes(65537);
    BOOST_CHECK(!validateBackupManifest(m));
    m.configuration.size = bytes(100);
    m.objects.count = NodeCount::from(100001).value();
    BOOST_CHECK(!validateBackupManifest(m));
}
BOOST_AUTO_TEST_CASE(result_keeps_unknown_commit_separate_from_publication) {
    MaintenanceResult<void> r = MaintenanceFailure{ MaintenanceError::CommitOutcomeUnknown, MaintenanceStage::Recover, id<OperationId>(), MaintenanceOutcome::Unknown, MaintenanceReportStatus::Incomplete, {}, id<MaintenanceActionId>() };
    BOOST_REQUIRE(!r);
    BOOST_CHECK(r.error().code == MaintenanceError::CommitOutcomeUnknown);
    BOOST_CHECK(r.error().outcome == MaintenanceOutcome::Unknown);
    BOOST_REQUIRE(r.error().action);
}

BOOST_AUTO_TEST_CASE(action_evidence_rejects_cross_binding_stale_unknown_and_inconsistent_files) {
    auto p = plan();
    auto a = p.actions.front();
    MaintenanceEvidenceBinding b{ id<InstanceId>(), id<MaintenanceRunId>(), p.id, hash(), a.id, {} };
    MaintenanceActionEvidence e{ b, MaintenanceEvidenceQuality::Durable, {}, hash() };
    BOOST_REQUIRE(validateMaintenanceEvidence(b, a, e));
    auto bad = e;
    bad.binding.run = id<MaintenanceRunId>('2');
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    bad = e;
    bad.binding.instance = id<InstanceId>('2');
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    bad = e;
    bad.binding.planDigest = hash('b');
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    bad = e;
    bad.binding.actionRevision = Revision::from(2).value();
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    for (auto quality : { MaintenanceEvidenceQuality::Observed, MaintenanceEvidenceQuality::Unknown }) {
        bad = e;
        bad.quality = quality;
        BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    }
    a.payload = ResumeDeletingBlob{ id<BlobId>(), {}, hash() };
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, e));
    e.files.push_back({ id<BlobId>(), {}, {} });
    BOOST_REQUIRE(validateMaintenanceEvidence(b, a, e));
    bad = e;
    bad.files[0].sha256 = hash();
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    bad = e;
    bad.files.push_back(bad.files.front());
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
}
BOOST_AUTO_TEST_CASE(file_evidence_checks_action_specific_postconditions) {
    auto a = plan().actions.front();
    FileEvidence before{ 1, 2, bytes(7), 3, 4 };
    a.payload = TruncateUnconfirmedTail{ id<UploadId>(), {}, UploadOffset::from(3).value(), before };
    MaintenanceEvidenceBinding b{ id<InstanceId>(), id<MaintenanceRunId>(), id<MaintenancePlanId>(), hash(), a.id, {} };
    MaintenanceActionEvidence e{ b, MaintenanceEvidenceQuality::Durable, { { id<UploadId>(), FileEvidence{ 1, 2, bytes(3), 5, 6 }, {} } }, hash() };
    BOOST_REQUIRE(validateMaintenanceEvidence(b, a, e));
    auto bad = e;
    bad.files[0].file->inode = 9;
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    bad = e;
    bad.files[0].file->size = bytes(7);
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    a.payload = QuarantineOrphan{ id<UploadId>(), id<QuarantineId>(), before, hash() };
    e.files = { { id<UploadId>(), {}, {} }, { id<QuarantineId>(), before, hash() } };
    BOOST_REQUIRE(validateMaintenanceEvidence(b, a, e));
    bad = e;
    bad.files[0].file = before;
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    bad = e;
    bad.files[1].sha256 = hash('b');
    BOOST_CHECK(!validateMaintenanceEvidence(b, a, bad));
    MaintenanceActionRecord r{ {}, a, MaintenanceActionState::Applied, e, UtcTime{}, UtcTime{}, Revision::from(2).value() };
    BOOST_REQUIRE(validateMaintenanceActionRecord(r));
    r.revision = Revision::from(3).value();
    BOOST_CHECK(!validateMaintenanceActionRecord(r));
    r.state = MaintenanceActionState::Prepared;
    BOOST_CHECK(!validateMaintenanceActionRecord(r));
}
