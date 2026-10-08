#include <apps/anof/Serve.h>
#include <CLI/CLI.hpp>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    try {
        CLI::App application{"ANOF personal/family cloud drive"};
        application.require_subcommand(1, 1);
        auto* command = application.add_subcommand("serve", "Serve an existing local development instance in the foreground");
        std::string root;
        command->add_option("--root", root, "Absolute existing instance root")->required();
        try {
            application.parse(argc, argv);
        } catch (const CLI::CallForHelp& help) {
            return application.exit(help);
        } catch (const CLI::ParseError&) {
            std::fputs("Invalid command line. Use --help.\n", stderr);
            return 2;
        }
        return anof::app::serve(root);
    } catch (...) {
        // 不传播第三方异常中可能含有的文件内容；fatal socket 失败不经过这里。
        std::fputs("ANOF_ERROR stage=process code=INTERNAL_ERROR\n", stderr);
        return 4;
    }
}
