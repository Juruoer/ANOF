#include <apps/anof/Init.h>
#include <anof/application/maintenance/PrepareInstance.h>
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
}
Result<TransferInitOptions> parseInitOptions(const InitArguments& arguments) {
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
    auto budget = parseTransferDecimal(arguments.contentBudget);
    auto minimum = parseTransferDecimal(arguments.diskSafetyMinimum);
    auto percent = parseTransferDecimal(arguments.diskSafetyPercent);
    if (!budget || !minimum || !percent || !SafetyPercent::from(percent.value().value())) return ErrorCode::InvalidArgument;
    InstanceConfiguration configuration;
    configuration.diskSafety = {minimum.value(), SafetyPercent::from(percent.value().value()).value()};
    configuration.developmentHttp = arguments.developmentHttp;
    if (!validateInitialQuotas(limits) || !validateInstanceConfiguration(configuration)) return ErrorCode::InvalidArgument;
    return TransferInitOptions{InitOptions{arguments.root, std::move(login).value(), std::move(name).value(), limits,
        UserLimit::from(count.value()).value(), configuration}, budget.value()};
}
}  // namespace anof::app
