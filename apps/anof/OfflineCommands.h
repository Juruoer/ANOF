#pragma once
#include <memory>
namespace CLI {
class App;
}
namespace anof::app {
class OfflineCommands {
public:
    explicit OfflineCommands(CLI::App&);
    ~OfflineCommands();
    bool selected() const;
    int run(bool json);

private:
    struct State;
    std::unique_ptr<State> state_;
};
}  // namespace anof::app
