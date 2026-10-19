#pragma once
#include <anof/client/CurlHttpTransport.h>
#include <apps/anof/ClientOutput.h>
namespace CLI {
class App;
}
namespace anof::app {
class TransferCommands {
public:
    explicit TransferCommands(CLI::App&);
    ~TransferCommands();
    std::optional<int> run(const std::string& directory, client::LockedProfile&, client::CurlHttpTransport&,
                           const std::atomic_bool*, ClientOutput&);

private:
    struct State;
    std::unique_ptr<State> state_;
};
}  // namespace anof::app
