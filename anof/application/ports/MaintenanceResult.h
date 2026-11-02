#pragma once
#include <anof/domain/maintenance/MaintenanceValues.h>

#include <filesystem>
#include <utility>

namespace anof {
enum class MaintenanceStage { Validate,
                              AcquireLocks,
                              Capture,
                              Inspect,
                              Plan,
                              PreserveOriginal,
                              Recover,
                              Export,
                              Normalize,
                              Copy,
                              Verify,
                              Publish,
                              Report };
enum class MaintenanceError { InvalidArgument,
                              Busy,
                              UnsupportedFormat,
                              UnsafePath,
                              AlreadyExists,
                              InvalidPlan,
                              StalePlan,
                              IntegrityError,
                              CheckIncomplete,
                              QuarantinePresent,
                              MaintenancePending,
                              StorageFull,
                              IoError,
                              CommitOutcomeUnknown,
                              PublicationOutcomeUnknown,
                              Cancelled,
                              InternalError };
enum class MaintenanceOutcome { NotStarted,
                                Confirmed,
                                Unknown };
struct MaintenanceFailure {
    MaintenanceError code;
    MaintenanceStage stage;
    OperationId operationId;
    MaintenanceOutcome outcome;
    MaintenanceReportStatus reportStatus = MaintenanceReportStatus::Incomplete;
    std::optional<std::filesystem::path> artifact;  // 只向本地操作者返回，不进HTTP或集中日志。
    std::optional<MaintenanceActionId> action;
};
template <class T>
class [[nodiscard]] MaintenanceResult {
public:
    MaintenanceResult(T value) : value_(std::move(value)) {}
    MaintenanceResult(MaintenanceFailure failure) : value_(std::move(failure)) {}
    explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
    T& value() & { return std::get<T>(value_); }
    const T& value() const& { return std::get<T>(value_); }
    T&& value() && { return std::get<T>(std::move(value_)); }
    const MaintenanceFailure& error() const { return std::get<MaintenanceFailure>(value_); }

private:
    std::variant<T, MaintenanceFailure> value_;
};
template <>
class [[nodiscard]] MaintenanceResult<void> {
public:
    MaintenanceResult() = default;
    MaintenanceResult(MaintenanceFailure failure) : failure_(std::move(failure)) {}
    explicit operator bool() const noexcept { return !failure_; }
    const MaintenanceFailure& error() const { return failure_.value(); }

private:
    std::optional<MaintenanceFailure> failure_;
};
}  // namespace anof
