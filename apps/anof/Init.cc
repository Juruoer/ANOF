#include <anof/application/maintenance/PrepareInstance.h>
#include <apps/anof/Init.h>
#include <apps/anof/TerminalPassword.h>
#include <anof/runtime/InstanceInitialization.h>
#include <cstdio>
#include <limits>

namespace anof::app {
namespace {
Result<std::int64_t> number(const std::string& text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return ErrorCode::InvalidArgument;
    std::int64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9' || value > (std::numeric_limits<std::int64_t>::max() - (c - '0')) / 10)
            return ErrorCode::InvalidArgument;
        value = value * 10 + c - '0';
    }
    return value;
}
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
Result<InitOptions> parseInitOptions(const InitArguments& arguments) {
    auto login = LoginName::parse(arguments.login);
    auto name = DisplayName::parse(arguments.displayName);
    auto count = number(arguments.maxUsers);
    if (!login || !name || !count || !UserLimit::from(count.value()) || !std::filesystem::path(arguments.root).is_absolute())
        return ErrorCode::InvalidArgument;
    std::array<std::int64_t, 6> quota{};
    for (std::size_t i = 0; i < quota.size(); ++i) {
        auto value = number(arguments.quotas[i]);
        if (!value) return value.error();
        quota[i] = value.value();
    }
    InitialQuotas limits{{ByteCount::from(quota[0]).value(), NodeCount::from(quota[1]).value()},
        {ByteCount::from(quota[2]).value(), NodeCount::from(quota[3]).value()},
        {ByteCount::from(quota[4]).value(), NodeCount::from(quota[5]).value()}};
    InstanceConfiguration configuration;
    configuration.developmentHttp = arguments.developmentHttp;
    if (!validateInitialQuotas(limits) || !validateInstanceConfiguration(configuration)) return ErrorCode::InvalidArgument;
    return InitOptions{arguments.root, std::move(login).value(), std::move(name).value(), limits,
        UserLimit::from(count.value()).value(), configuration};
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
