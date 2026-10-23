#pragma once
#include <apps/anof/ClientOutput.h>
namespace CLI { class App; }
namespace anof::app {
class DriveCommands {
public:
    explicit DriveCommands(CLI::App&);
    ~DriveCommands();
    bool validate(ClientOutput&);
    std::optional<int> run(client::LockedProfile&, client::HttpTransport&, const std::atomic_bool*, ClientOutput&);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
