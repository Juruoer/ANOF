#define BOOST_TEST_MODULE TerminalPassword
#include <boost/test/unit_test.hpp>
#include <apps/anof/TerminalPassword.h>
#include <apps/anof/Init.h>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <string>

namespace {
struct TerminalChild {
    int master = -1, slave = -1;
    pid_t pid = -1;
    termios original{};
    std::string output;
    explicit TerminalChild(const anof::app::InitArguments* arguments = nullptr) {
        master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
        BOOST_REQUIRE(master >= 0);
        BOOST_REQUIRE_EQUAL(::grantpt(master), 0);
        BOOST_REQUIRE_EQUAL(::unlockpt(master), 0);
        slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY | O_CLOEXEC);
        BOOST_REQUIRE(slave >= 0);
        BOOST_REQUIRE_EQUAL(::tcgetattr(slave, &original), 0);
        pid = ::fork();
        BOOST_REQUIRE(pid >= 0);
        if (pid == 0) {
            ::alarm(5);
            if (::setsid() < 0 || ::ioctl(slave, TIOCSCTTY, 0) != 0) ::_exit(90);
            if (arguments) ::_exit(anof::app::initialize(*arguments));
            auto result = anof::app::readConfirmedPassword();
            ::_exit(result && result.value().view() == "a secret password" ? 0 : 2);
        }
    }
    ~TerminalChild() {
        if (pid > 0) { ::kill(pid, SIGKILL); while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }
        if (slave >= 0) ::close(slave);
        if (master >= 0) ::close(master);
    }
    void until(const std::string& text) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (output.find(text) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
            pollfd fd{master, POLLIN, 0}; ::poll(&fd, 1, 20);
            char bytes[256]; const auto n = ::read(master, bytes, sizeof bytes);
            if (n > 0) output.append(bytes, static_cast<std::size_t>(n));
            BOOST_REQUIRE(output.size() < 4096);
        }
        BOOST_REQUIRE(output.find(text) != std::string::npos);
    }
    void send(const std::string& text) { BOOST_REQUIRE_EQUAL(::write(master, text.data(), text.size()), static_cast<ssize_t>(text.size())); }
    void hidden() {
        termios state{}; BOOST_REQUIRE_EQUAL(::tcgetattr(slave, &state), 0);
        BOOST_CHECK((state.c_lflag & (ECHO | ECHONL)) == 0);
    }
    void finish(int expected) {
        int status = 0;
        pid_t result;
        do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
        BOOST_REQUIRE_EQUAL(result, pid); pid = -1;
        BOOST_REQUIRE(WIFEXITED(status)); BOOST_CHECK_EQUAL(WEXITSTATUS(status), expected);
        termios state{}; BOOST_REQUIRE_EQUAL(::tcgetattr(slave, &state), 0);
        BOOST_CHECK_EQUAL(state.c_lflag, original.c_lflag);
        char bytes[256]; ssize_t n;
        while ((n = ::read(master, bytes, sizeof bytes)) > 0) output.append(bytes, static_cast<std::size_t>(n));
        BOOST_CHECK(output.find("a secret password") == std::string::npos);
    }
};
}
BOOST_AUTO_TEST_CASE(confirm_without_echo_and_restore) {
    TerminalChild child;
    child.until("Administrator password: "); child.hidden(); child.send("a secret password\n");
    child.until("Confirm password: "); child.hidden(); child.send("a secret password\n"); child.finish(0);
}
BOOST_AUTO_TEST_CASE(mismatch_and_interrupt_restore_terminal) {
    {
        TerminalChild child;
        child.until("Administrator password: "); child.send("a secret password\n");
        child.until("Confirm password: "); child.send("another password\n"); child.finish(2);
    }
    {
        TerminalChild child;
        child.until("Administrator password: "); child.hidden();
        BOOST_REQUIRE_EQUAL(::kill(child.pid, SIGINT), 0); child.finish(2);
    }
}
BOOST_AUTO_TEST_CASE(oversized_line_is_rejected_without_echo) {
    TerminalChild child;
    child.until("Administrator password: "); child.send(std::string(1025, 'x') + "\n"); child.finish(2);
    BOOST_CHECK(child.output.find(std::string(20, 'x')) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(init_command_creates_real_instance_from_terminal_input) {
    char pattern[] = "/tmp/anof-init-command-XXXXXX";
    const auto* root = ::mkdtemp(pattern);
    BOOST_REQUIRE(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } cleanup{root};
    anof::app::InitArguments arguments;
    arguments.contentBudget = "1048576";
    arguments.root = root; arguments.login = "admin"; arguments.displayName = "Test administrator";
    arguments.developmentHttp = true;
    arguments.quotas = {"1048576", "10", "1048576", "10", "1048576", "10"};
    TerminalChild child(&arguments);
    child.until("Administrator password: "); child.hidden(); child.send("a secret password\n");
    child.until("Confirm password: "); child.send("a secret password\n"); child.finish(0);
    BOOST_CHECK(std::filesystem::exists(cleanup.root / ".anof/instance.json"));
    BOOST_CHECK(std::filesystem::exists(cleanup.root / ".anof/metadata.sqlite3"));
    BOOST_CHECK(!std::filesystem::exists(cleanup.root / ".anof/initialization.pending"));
}
