#define BOOST_TEST_MODULE MaintenanceOutput
#include <apps/anof/MaintenanceOutput.h>

#include <boost/test/unit_test.hpp>

using namespace anof;
using namespace anof::app;
namespace {
MaintenanceFailure failed(MaintenanceError code, MaintenanceOutcome outcome) {
    return { code, MaintenanceStage::Report, OperationId::parse("10000000-0000-4000-8000-000000000001").value(), outcome, MaintenanceReportStatus::Incomplete, {}, {} };
}
}  // namespace
BOOST_AUTO_TEST_CASE(unknown_effects_take_precedence_over_io_and_cancellation) {
    BOOST_CHECK_EQUAL(maintenanceExitCode(failed(MaintenanceError::IoError, MaintenanceOutcome::Unknown)), 6);
    BOOST_CHECK_EQUAL(maintenanceExitCode(failed(MaintenanceError::Cancelled, MaintenanceOutcome::Confirmed)), 6);
    BOOST_CHECK_EQUAL(maintenanceExitCode(failed(MaintenanceError::Cancelled, MaintenanceOutcome::NotStarted)), 130);
    BOOST_CHECK_EQUAL(maintenanceExitCode(failed(MaintenanceError::PublicationOutcomeUnknown, MaintenanceOutcome::NotStarted)), 6);
}
BOOST_AUTO_TEST_CASE(confirmed_effects_survive_report_failure) {
    auto f = failed(MaintenanceError::IoError, MaintenanceOutcome::Confirmed);
    f.artifact = "/tmp/anof-original";
    auto data = maintenanceFailureData(f);
    BOOST_CHECK_EQUAL(maintenanceExitCode(f), 5);
    BOOST_CHECK_EQUAL(data.at("outcome").get<std::string>(), "confirmed");
    BOOST_CHECK_EQUAL(data.at("stage").get<std::string>(), "report");
    BOOST_CHECK_EQUAL(data.at("artifact").get<std::string>(), "/tmp/anof-original");
    BOOST_CHECK_EQUAL(data.at("operation_id").get<std::string>(), f.operationId.toString());
}
BOOST_AUTO_TEST_CASE(findings_and_known_refusals_use_distinct_exit_codes) {
    BOOST_CHECK_EQUAL(maintenanceExitCode(failed(MaintenanceError::IntegrityError, MaintenanceOutcome::NotStarted)), 4);
    BOOST_CHECK_EQUAL(maintenanceExitCode(failed(MaintenanceError::CheckIncomplete, MaintenanceOutcome::NotStarted)), 4);
    BOOST_CHECK_EQUAL(maintenanceExitCode(failed(MaintenanceError::StalePlan, MaintenanceOutcome::NotStarted)), 3);
    BOOST_CHECK_EQUAL(maintenanceExitCode(failed(MaintenanceError::Busy, MaintenanceOutcome::NotStarted)), 3);
}

#include <apps/anof/MaintenancePlanJson.h>
BOOST_AUTO_TEST_CASE(all_plan_actions_round_trip_without_quantity_or_identity_coercion) {
    auto h = Sha256::parse(std::string(64, 'a')).value();
    const std::string uuid = "10000000-0000-4000-8000-000000000001";
    auto upload = UploadId::parse(uuid).value();
    auto blob = BlobId::parse(uuid).value();
    FileEvidence file{ UINT64_MAX, UINT64_MAX, ByteCount::from(3).value(), INT64_MIN, INT64_MAX };
    MaintenancePlan p{ MaintenancePlanId::parse(uuid).value(), InstanceId::parse(uuid).value(), 5, 5, 1, { h, h }, InspectionMode::Full, {}, {}, MaintenanceFingerprint{ h, h } };
    std::vector<MaintenanceActionPayload> values{ RecomputeAccounting{ h }, TruncateUnconfirmedTail{ upload, {}, UploadOffset::from(2).value(), file }, ResumeUpload{ upload, {}, h }, ResumeDeletingBlob{ blob, {}, h }, QuarantineOrphan{ upload, QuarantineId::parse(uuid).value(), file, h }, MarkBlobUnavailable{ blob, {}, { UnavailableReason::Length, file, {} } }, VerifyRestoredBlob{ blob, {}, file, h } };
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto a = uuid;
        a.back() = static_cast<char>('1' + i);
        p.actions.push_back({ MaintenanceActionId::parse(a).value(), values[i], h });
    }
    auto data = maintenancePlanData(p);
    auto text = data.dump();
    auto decoded = parseMaintenancePlan(text);
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(maintenancePlanData(decoded.value()) == data);
    data["actions"][1]["payload"]["generation"] = 1;
    BOOST_CHECK(!parseMaintenancePlan(data.dump()));
    BOOST_CHECK(!parseMaintenancePlan(std::string(17, '[') + "0" + std::string(17, ']')));
    text.insert(text.size() - 1, ",\"mode\":\"full\"");
    BOOST_CHECK(!parseMaintenancePlan(text));
    BOOST_CHECK(!parseMaintenancePlan(std::string("{\"invalid\":\"") + char(0xff) + "\"}"));
}
