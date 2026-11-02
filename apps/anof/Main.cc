#include <apps/anof/Serve.h>
#include <apps/anof/RuntimeDependencies.h>
#include <apps/anof/OfflineCommands.h>
#include <csignal>
#include <apps/anof/Init.h>
#include <apps/anof/ClientCommands.h>
#include <CLI/CLI.hpp>
#include <anof/build/Version.h>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (const char* error = anof::app::runtimeDependencyFailure()) {
        std::fprintf(stderr, "ANOF_ERROR stage=dependencies code=%s\n", error);
        return 4;
    }
    std::signal(SIGPIPE, SIG_IGN);
    try {
        CLI::App application{"ANOF personal/family cloud drive"};
        const std::string version = std::string("ANOF ") + anof::build::version;
        application.set_version_flag("--version", version);
        application.require_subcommand(1, 1);
        auto* command = application.add_subcommand("serve", "Serve an existing instance in the foreground");
        command->set_version_flag("--version", version);
        std::string root;
        command->add_option("--root", root, "Absolute existing instance root")->required();
        anof::app::InitArguments inputs;
        bool finish = false;
        auto* init = application.add_subcommand("init", "Initialize a new instance or finish a committed initialization");
        init->add_option("--root", inputs.root, "Absolute existing empty owned 0700 root")->required();
        init->add_flag("--finish", finish, "Finish the same committed initialization without changing inputs");
        std::vector<CLI::Option*> initialOptions;
        initialOptions.push_back(init->add_option("--admin-login", inputs.login));
        initialOptions.push_back(init->add_option("--admin-display-name", inputs.displayName));
        const std::array<const char*, 6> quotaNames{"--private-bytes", "--private-nodes", "--public-bytes",
            "--public-nodes", "--contribution-bytes", "--contribution-nodes"};
        for (std::size_t i = 0; i < quotaNames.size(); ++i)
            initialOptions.push_back(init->add_option(quotaNames[i], inputs.quotas[i], "Explicit nonnegative decimal quota"));
        initialOptions.push_back(init->add_option("--max-users", inputs.maxUsers, "Total user limit, default 10"));
        initialOptions.push_back(init->add_flag("--development-http", inputs.developmentHttp, "Explicit loopback development HTTP opt-in"));
        initialOptions.push_back(init->add_option("--content-budget-bytes", inputs.contentBudget, "Required physical content budget"));
        initialOptions.push_back(init->add_option("--disk-safety-min-bytes", inputs.diskSafetyMinimum, "Minimum free disk margin, default 1073741824"));
        initialOptions.push_back(init->add_option("--disk-safety-percent", inputs.diskSafetyPercent, "Free disk margin percent, default 5"));
        anof::app::ClientCommands client(application);
        anof::app::OfflineCommands offline(application);
        try {
            application.parse(argc, argv);
        } catch (const CLI::CallForVersion&) {
            if (std::printf("%s\n", version.c_str()) < 0 || std::fflush(stdout) != 0) return 5;
            return 0;
        } catch (const CLI::CallForHelp& help) {
            return (*init || *command || offline.selected()) ? application.exit(help) : client.help(application, help);
        } catch (const CLI::ParseError&) {
            return client.invalidArguments();
        }
        if (!client.validOptionOrder(argc, argv) || ((*init || *command) && client.hasGlobalOptions())) return client.invalidArguments();
        if (*init) {
            if (finish) {
                for (auto* option : initialOptions) if (option->count()) {
                    std::fputs("--finish accepts only --root.\n", stderr);
                    return 2;
                }
                return anof::app::finish(inputs.root);
            }
            return anof::app::initialize(inputs);
        }
        if (*command) return anof::app::serve(root);
        if (offline.selected()) {
            if (client.hasRemoteOptions()) return client.invalidArguments();
            return offline.run(client.jsonOutput());
        }
        return client.run();
    } catch (...) {
        // 不传播第三方异常中可能含有的文件内容；fatal socket 失败不经过这里。
        std::fputs("ANOF_ERROR stage=process code=INTERNAL_ERROR\n", stderr);
        return 4;
    }
}
