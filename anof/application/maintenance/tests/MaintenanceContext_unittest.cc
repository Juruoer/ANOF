#define BOOST_TEST_MODULE AnofMaintenanceContext
#include <anof/application/ports/MaintenanceFileStore.h>
#include <anof/application/ports/MetadataInspection.h>
#include <anof/application/ports/MetadataRepair.h>

#include <boost/test/unit_test.hpp>
#include <type_traits>

using namespace anof;
namespace {
OperationId operation(bool second = false) {
    return OperationId::parse(second ? "00000000-0000-4000-8000-000000000002"
                                     : "00000000-0000-4000-8000-000000000001")
        .value();
}
// 检查全部有取消语义的返回对象方法都显式接收逐调用上下文，防止遗漏旧签名。
template <class>
struct HasContext : std::false_type {};
template <class R, class C, class... Args>
struct HasContext<R (C::*)(const MaintenanceContext&, Args...)> : std::true_type {};
#define CHECK_CONTEXT(Type, Method) static_assert(HasContext<decltype(&Type::Method)>::value)
CHECK_CONTEXT(MetadataRepairSession, findRun);
CHECK_CONTEXT(MetadataRepairSession, actions);
CHECK_CONTEXT(MetadataRepairSession, beginWrite);
CHECK_CONTEXT(MetadataRepairSession, verifyCompletedAction);
CHECK_CONTEXT(MetadataRepairTransaction, insertRun);
CHECK_CONTEXT(MetadataRepairTransaction, prepareAction);
CHECK_CONTEXT(MetadataRepairTransaction, advanceAction);
CHECK_CONTEXT(MetadataRepairTransaction, completeRun);
CHECK_CONTEXT(MetadataRepairTransaction, markUnavailable);
CHECK_CONTEXT(MetadataRepairTransaction, restoreAvailable);
CHECK_CONTEXT(MetadataRepairTransaction, recomputeAccounting);
CHECK_CONTEXT(MetadataRepairTransaction, prepareQuarantine);
CHECK_CONTEXT(MetadataRepairTransaction, holdQuarantine);
CHECK_CONTEXT(MetadataRepairTransaction, appendAudit);
CHECK_CONTEXT(MetadataRepairTransaction, commit);
CHECK_CONTEXT(MetadataInspectionReader, identity);
CHECK_CONTEXT(MetadataInspectionReader, findings);
CHECK_CONTEXT(MetadataInspectionReader, objects);
CHECK_CONTEXT(MetadataInspectionReader, quarantine);
CHECK_CONTEXT(MetadataInspectionReader, uploads);
CHECK_CONTEXT(MetadataInspectionReader, runs);
CHECK_CONTEXT(MetadataInspectionReader, accounting);
CHECK_CONTEXT(MetadataInspectionReader, summary);
CHECK_CONTEXT(MetadataInspectionReader, repairInvariant);
CHECK_CONTEXT(MaintenanceInventory, next);
CHECK_CONTEXT(MaintenanceBackupSource, objects);
#undef CHECK_CONTEXT
static_assert(std::is_same_v<decltype(&MaintenanceBackupSource::manifest),
                             const BackupManifest& (MaintenanceBackupSource::*)() const noexcept>);
}  // namespace

BOOST_AUTO_TEST_CASE(operation_binding_precedes_cancellation_and_reports_call_identity) {
    const std::atomic<bool> cancelled{ true };
    MaintenanceContext call{ operation(true), cancelled };
    const auto result = validateMaintenanceCall(call, operation(), MaintenanceStage::Recover);
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == MaintenanceError::InvalidArgument);
    BOOST_CHECK(result.error().stage == MaintenanceStage::Recover);
    BOOST_CHECK(result.error().operationId == call.operationId);
    BOOST_CHECK(result.error().outcome == MaintenanceOutcome::NotStarted);
    BOOST_CHECK(result.error().reportStatus == MaintenanceReportStatus::Incomplete);
    BOOST_CHECK(!result.error().artifact);
    BOOST_CHECK(!result.error().action);
}

BOOST_AUTO_TEST_CASE(each_call_uses_its_own_cancellation_flag) {
    const auto bound = operation();
    {
        const std::atomic<bool> cancelled{ true };
        const auto result = validateMaintenanceCall({ bound, cancelled }, bound, MaintenanceStage::Inspect);
        BOOST_REQUIRE(!result);
        BOOST_CHECK(result.error().code == MaintenanceError::Cancelled);
        BOOST_CHECK(result.error().outcome == MaintenanceOutcome::NotStarted);
    }
    std::atomic<bool> nextFlag{ false };
    MaintenanceContext next{ bound, nextFlag };
    BOOST_CHECK(validateMaintenanceCall(next, bound, MaintenanceStage::Inspect));
    nextFlag.store(true);
    const auto result = validateMaintenanceCall(next, bound, MaintenanceStage::Inspect);
    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error().code == MaintenanceError::Cancelled);
    BOOST_CHECK(result.error().operationId == bound);
    BOOST_CHECK(result.error().stage == MaintenanceStage::Inspect);
}

BOOST_AUTO_TEST_CASE(returned_failure_owns_its_values_after_context_destruction) {
    const auto failure = [] {
        const std::atomic<bool> cancelled{ true };
        return validateMaintenanceCall({ operation(), cancelled }, operation(), MaintenanceStage::Verify);
    }();
    BOOST_REQUIRE(!failure);
    BOOST_CHECK(failure.error().operationId == operation());
    BOOST_CHECK(failure.error().stage == MaintenanceStage::Verify);
    BOOST_CHECK(failure.error().code == MaintenanceError::Cancelled);
}
