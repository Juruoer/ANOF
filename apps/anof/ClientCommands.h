#pragma once
#include <memory>
namespace CLI { class App; class CallForHelp; }
namespace anof::app {
class ClientCommands {
public:
    explicit ClientCommands(CLI::App&);
    ~ClientCommands();
    bool hasGlobalOptions() const;
    bool validOptionOrder(int argc, char** argv) const;
    int invalidArguments() const;
    int help(const CLI::App&, const CLI::CallForHelp&) const;
    int run();
private:
    struct State;
    std::unique_ptr<State> state_;
};
}  // namespace anof::app
