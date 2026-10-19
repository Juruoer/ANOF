#pragma once
#include <anof/application/maintenance/TransferInitialization.h>
#include <array>
#include <string>

namespace anof::app {
struct InitArguments {
    std::string root, login, displayName;
    // private bytes/nodes, public bytes/nodes, contribution bytes/nodes，均无隐藏默认。
    std::array<std::string, 6> quotas;
    std::string maxUsers = "10";
    bool developmentHttp = false;
    std::string contentBudget; // 必填，无默认预算。
    std::string diskSafetyMinimum = "1073741824";
    std::string diskSafetyPercent = "5";
};
Result<TransferInitOptions> parseInitOptions(const InitArguments& arguments);
int initialize(const InitArguments& arguments);
int finish(const std::filesystem::path& root);
}  // namespace anof::app
