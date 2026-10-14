#include <apps/anof/Serve.h>
#include <apps/anof/Init.h>
#include <apps/anof/ClientCommands.h>
#include <CLI/CLI.hpp>

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        CLI::App application{"ANOF personal/family cloud drive"};
        application.require_subcommand(1, 1);
        auto* command = application.add_subcommand("serve", "Serve an existing local development instance in the foreground");
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
        anof::app::ClientCommands client(application);
        try {
            application.parse(argc, argv);
        } catch (const CLI::CallForHelp& help) {
            return (*init || *command) ? application.exit(help) : client.help(application, help);
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
        return client.run();
    } catch (...) {
        // 不传播第三方异常中可能含有的文件内容；fatal socket 失败不经过这里。
        std::fputs("ANOF_ERROR stage=process code=INTERNAL_ERROR\n", stderr);
        return 4;
    }
}
