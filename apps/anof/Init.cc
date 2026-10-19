#include <anof/application/maintenance/PrepareInstance.h>
#include <apps/anof/Init.h>
#include <apps/anof/TerminalPassword.h>
#include <anof/runtime/InstanceInitialization.h>
#include <cstdio>
#include <limits>

namespace anof::app {
namespace {
const char* errorName(InitError error) {
    switch (error) {
    case InitError::InvalidInput: return "INVALID_INPUT";
    case InitError::NotFound: return "NOT_FOUND";
    case InitError::AlreadyExists: return "ALREADY_EXISTS";
    case InitError::Busy: return "BUSY";
    case InitError::IncompleteInitialization: return "INCOMPLETE_INITIALIZATION";
    case InitError::UnsupportedFormat: return "UNSUPPORTED_FORMAT";
    case InitError::IntegrityError: return "INTEGRITY_ERROR";
    case InitError::StorageFull: return "STORAGE_FULL";
    case InitError::CommitOutcomeUnknown: return "COMMIT_OUTCOME_UNKNOWN";
    case InitError::PublicationOutcomeUnknown: return "PUBLICATION_OUTCOME_UNKNOWN";
    case InitError::IoError: return "IO_ERROR";
    case InitError::InternalError: return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}
const char* stageName(InitStage stage) {
    switch (stage) {
    case InitStage::Validate: return "validate";
    case InitStage::HashPassword: return "hash_password";
    case InitStage::CreateDirectory: return "create_directory";
    case InitStage::AcquireLock: return "acquire_lock";
    case InitStage::WritePending: return "write_pending";
    case InitStage::WriteConfiguration: return "write_configuration";
    case InitStage::CreateDatabase: return "create_database";
    case InitStage::Bootstrap: return "bootstrap";
    case InitStage::Verify: return "verify";
    case InitStage::PublishFormat: return "publish_format";
    case InitStage::ClearPending: return "clear_pending";
    }
    return "validate";
}
int report(InitResult<InitReceipt> result) {
    if (result) { std::puts("ANOF_INITIALIZED"); return 0; }
    std::fprintf(stderr, "ANOF_ERROR stage=%s code=%s\n", stageName(result.error().stage), errorName(result.error().code));
    return result.error().code == InitError::InvalidInput ? 2 : 3;
}
}
int initialize(const InitArguments& arguments) {
    auto options = parseInitOptions(arguments);
    if (!options) return report(InitFailure{InitError::InvalidInput, InitStage::Validate});
    auto password = readConfirmedPassword();
    if (!password) return report(InitFailure{InitError::InvalidInput, InitStage::Validate});
    return report(runtime::initializeInstance(std::move(options).value(), std::move(password).value()));
}
int finish(const std::filesystem::path& root) {
    return report(runtime::finishInitialization(root));
}
}  // namespace anof::app
