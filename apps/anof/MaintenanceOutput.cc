#include <apps/anof/MaintenanceOutput.h>

namespace anof::app {
int maintenanceExitCode(const MaintenanceFailure& f) noexcept {
    if (f.outcome == MaintenanceOutcome::Unknown || f.code == MaintenanceError::CommitOutcomeUnknown ||
        f.code == MaintenanceError::PublicationOutcomeUnknown) return 6;
    switch (f.code) {
    case MaintenanceError::Cancelled:
        return f.outcome == MaintenanceOutcome::NotStarted ? 130 : 6;
    case MaintenanceError::IntegrityError:
    case MaintenanceError::CheckIncomplete:
    case MaintenanceError::QuarantinePresent:
        return 4;
    case MaintenanceError::StorageFull:
    case MaintenanceError::IoError:
    case MaintenanceError::InternalError:
        return 5;
    default:
        return 3;
    }
}
const char* maintenanceErrorName(MaintenanceError v) noexcept {
    switch (v) {
    case MaintenanceError::InvalidArgument:
        return "INVALID_ARGUMENT";
    case MaintenanceError::Busy:
        return "BUSY";
    case MaintenanceError::UnsupportedFormat:
        return "UNSUPPORTED_FORMAT";
    case MaintenanceError::UnsafePath:
        return "UNSAFE_PATH";
    case MaintenanceError::AlreadyExists:
        return "ALREADY_EXISTS";
    case MaintenanceError::InvalidPlan:
        return "INVALID_PLAN";
    case MaintenanceError::StalePlan:
        return "STALE_PLAN";
    case MaintenanceError::IntegrityError:
        return "INTEGRITY_ERROR";
    case MaintenanceError::CheckIncomplete:
        return "CHECK_INCOMPLETE";
    case MaintenanceError::QuarantinePresent:
        return "QUARANTINE_PRESENT";
    case MaintenanceError::MaintenancePending:
        return "MAINTENANCE_PENDING";
    case MaintenanceError::StorageFull:
        return "STORAGE_FULL";
    case MaintenanceError::IoError:
        return "IO_ERROR";
    case MaintenanceError::CommitOutcomeUnknown:
        return "COMMIT_OUTCOME_UNKNOWN";
    case MaintenanceError::PublicationOutcomeUnknown:
        return "PUBLICATION_OUTCOME_UNKNOWN";
    case MaintenanceError::Cancelled:
        return "CANCELLED";
    case MaintenanceError::InternalError:
        return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}
const char* maintenanceStageName(MaintenanceStage v) noexcept {
    switch (v) {
    case MaintenanceStage::Validate:
        return "validate";
    case MaintenanceStage::AcquireLocks:
        return "acquire_locks";
    case MaintenanceStage::Capture:
        return "capture";
    case MaintenanceStage::Inspect:
        return "inspect";
    case MaintenanceStage::Plan:
        return "plan";
    case MaintenanceStage::PreserveOriginal:
        return "preserve_original";
    case MaintenanceStage::Recover:
        return "recover";
    case MaintenanceStage::Export:
        return "export";
    case MaintenanceStage::Normalize:
        return "normalize";
    case MaintenanceStage::Copy:
        return "copy";
    case MaintenanceStage::Verify:
        return "verify";
    case MaintenanceStage::Publish:
        return "publish";
    case MaintenanceStage::Report:
        return "report";
    }
    return "validate";
}
const char* maintenanceOutcomeName(MaintenanceOutcome v) noexcept {
    switch (v) {
    case MaintenanceOutcome::NotStarted:
        return "not_started";
    case MaintenanceOutcome::Confirmed:
        return "confirmed";
    case MaintenanceOutcome::Unknown:
        return "unknown";
    }
    return "unknown";
}
const char* maintenanceStatusName(MaintenanceReportStatus v) noexcept {
    switch (v) {
    case MaintenanceReportStatus::Healthy:
        return "healthy";
    case MaintenanceReportStatus::Findings:
        return "findings";
    case MaintenanceReportStatus::Incomplete:
        return "incomplete";
    }
    return "incomplete";
}
nlohmann::json maintenanceFailureData(const MaintenanceFailure& f) {
    return { { "type", "result" }, { "report_format", kMaintenanceReportFormat }, { "code", maintenanceErrorName(f.code) }, { "stage", maintenanceStageName(f.stage) }, { "operation_id", f.operationId.toString() }, { "outcome", maintenanceOutcomeName(f.outcome) }, { "report_status", maintenanceStatusName(f.reportStatus) }, { "artifact", f.artifact ? nlohmann::json(f.artifact->string()) : nlohmann::json(nullptr) }, { "action_id", f.action ? nlohmann::json(f.action->toString()) : nlohmann::json(nullptr) } };
}
}  // namespace anof::app
