#pragma once
#include <anof/application/maintenance/Initialization.h>
#include <array>
#include <string>

namespace anof::app {
struct InitArguments {
    std::string root, login, displayName;
    // private bytes/nodes, public bytes/nodes, contribution bytes/nodes，均无隐藏默认。
    std::array<std::string, 6> quotas;
    std::string maxUsers = "10";
    bool developmentHttp = false;
};
Result<InitOptions> parseInitOptions(const InitArguments& arguments);
int initialize(const InitArguments& arguments);
int finish(const std::filesystem::path& root);
}  // namespace anof::app
