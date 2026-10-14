#include <tests/diagnostics/ProcessSnapshot.h>
#include <tests/diagnostics/WaitTrace.h>
#define BOOST_TEST_MODULE ServeProcess
#include <boost/test/unit_test.hpp>
#include <tests/integration/InitializedInstance.h>
#include <anof/infrastructure/system/LinuxInstanceLock.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace anof;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Process {
    pid_t pid = -1;
    int output = -1;
    std::optional<int> status;
    std::string text;
    explicit Process(std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), ANOF_EXECUTABLE);
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        int pipe[2];
        if (::pipe2(pipe, O_CLOEXEC) != 0) throw std::runtime_error("pipe failed");
        pid = ::fork();
        if (pid == 0) {
            const rlimit noCore{0, 0};
            (void)::setrlimit(RLIMIT_CORE, &noCore);
            ::alarm(10);
            if (::dup2(pipe[1], STDOUT_FILENO) < 0 || ::dup2(pipe[1], STDERR_FILENO) < 0) ::_exit(120);
            ::close(pipe[0]); ::close(pipe[1]);
            ::execv(argv[0], argv.data());
            ::_exit(121);
        }
        ::close(pipe[1]);
        if (pid < 0) { ::close(pipe[0]); throw std::runtime_error("fork failed"); }
        output = pipe[0];
        if (::fcntl(output, F_SETFL, O_NONBLOCK) != 0) {
            ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); ::close(output);
            throw std::runtime_error("fcntl failed");
        }
    }
    ~Process() {
        if (!status && pid > 0) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
        if (output >= 0) ::close(output);
    }
    void read() {
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto n = ::read(output, buffer.data(), buffer.size());
            if (n > 0) {
                text.append(buffer.data(), static_cast<std::size_t>(n));
                if (text.size() > 256 * 1024) throw std::runtime_error("process output limit");
            } else if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) return;
            else if (errno == EINTR) continue;
            else throw std::runtime_error("read failed");
        }
    }
    bool wait(const std::function<bool()>& predicate, double seconds = 4) {
        const auto deadline = Clock::now() + std::chrono::duration<double>(seconds);
        do {
            read();
            if (!status) {
                int value;
                const auto child = ::waitpid(pid, &value, WNOHANG);
                if (child == pid) { status = value; read(); }
                else if (child < 0 && errno != EINTR) throw std::runtime_error("waitpid failed");
            }
            if (predicate()) return true;
            if (status) return false;
            pollfd descriptor{output, POLLIN, 0};
            (void)::poll(&descriptor, 1, 10);
        } while (Clock::now() < deadline);
        return false;
    }
    unsigned short ready() {
        BOOST_REQUIRE_MESSAGE(wait([&] { const auto start = text.find("ANOF_READY address=127.0.0.1 port="); return start != std::string::npos && text.find('\n', start) != std::string::npos; }), text);
        const auto pos = text.find("ANOF_READY address=127.0.0.1 port=");
        return static_cast<unsigned short>(std::stoul(text.substr(pos + std::string("ANOF_READY address=127.0.0.1 port=").size())));
    }
    void exited(int code) {
        anof::test::WaitTrace trace(__FILE__, __LINE__);
        trace.event("process_exit_wait_begin");
        const bool done = wait([&] { return status.has_value(); });
        trace.event(done ? "process_exited" : "process_exit_timeout", done);
        if (std::getenv("ANOF_SHUTDOWN_TRACE")) std::fprintf(stderr, "CHILD_TRACE pid=%ld\n%s\n", static_cast<long>(pid), text.c_str());
        if (!done && std::getenv("ANOF_SHUTDOWN_TRACE")) anof::test::processSnapshot(pid);
        BOOST_REQUIRE_MESSAGE(done, text);
        BOOST_REQUIRE_MESSAGE(WIFEXITED(*status), text);
        BOOST_CHECK_EQUAL(WEXITSTATUS(*status), code);
    }
    void stop(int signal) { BOOST_REQUIRE_EQUAL(::kill(pid, signal), 0); exited(0); }
};

struct Fixture {
    fs::path root;
    Fixture() {
        char name[] = "/tmp/anof-serve-process-XXXXXX";
        auto created = ::mkdtemp(name);
        if (!created) throw std::runtime_error("mkdtemp failed");
        root = created;
        BOOST_REQUIRE(anof::testing::initializeTestInstance(root));
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    void write(const char* file, const std::string& contents) {
        auto path = root / ".anof" / file;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << contents; stream.close();
        BOOST_REQUIRE(stream);
        BOOST_REQUIRE_EQUAL(::chmod(path.c_str(), 0600), 0);
    }
    std::vector<std::string> arguments() const { return {"serve", "--root", root.string()}; }
    void lockAvailable(bool expected) {
        auto locks = infrastructure::system::makeLinuxInstanceLockProvider();
        BOOST_REQUIRE(locks);
        auto lease = locks.value()->acquire(root);
        BOOST_CHECK_EQUAL(bool(lease), expected);
        if (!expected && !lease) BOOST_CHECK(lease.error().code == ErrorCode::Conflict);
    }
};
struct Client {
    int fd = -1;
    Process* process;
    Client(unsigned short port, Process& child) : process(&child) {
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
            ::close(fd); throw std::runtime_error("connect failed");
        }
    }
    ~Client() { ::close(fd); }
    void send(const std::string& text) {
        BOOST_REQUIRE_EQUAL(::send(fd, text.data(), text.size(), MSG_NOSIGNAL | MSG_DONTWAIT), static_cast<ssize_t>(text.size()));
    }
    std::string receive(bool interim = false) {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        std::string text;
        while (Clock::now() < deadline) {
            // 同时排空子进程日志，避免测试捕获管道阻塞服务的关闭路径。
            process->read();
            char buffer[4096];
            const auto n = ::recv(fd, buffer, sizeof buffer, MSG_DONTWAIT);
            if (n > 0) text.append(buffer, static_cast<std::size_t>(n));
            else if (n == 0) return text;
            else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) throw std::runtime_error("recv failed");
            if (text.size() > 65536) throw std::runtime_error("HTTP output limit");
            if (interim && text.find("100 Continue\r\n\r\n") != std::string::npos) return text;
            pollfd descriptor{fd, POLLIN, 0}; (void)::poll(&descriptor, 1, 10);
        }
        throw std::runtime_error("HTTP timeout");
    }
};

BOOST_AUTO_TEST_CASE(command_line_rejects_unsupported_or_incomplete_commands) {
    for (const auto& arguments : std::vector<std::vector<std::string>>{{}, {"init"}, {"serve"}, {"serve", "--root", "/tmp", "--disable-auth"}}) {
        Process process(arguments); process.exited(2);
        BOOST_CHECK(process.text.find("ANOF_READY") == std::string::npos);
    }
    Process help({"serve", "--help"}); help.exited(0);
    BOOST_CHECK(help.text.find("--root") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(real_preparation_http_sigterm_and_sigint_allow_restart) {
    Fixture fixture;
    for (int signal : {SIGTERM, SIGINT}) {
        Process process(fixture.arguments());
        const auto port = process.ready();
        fixture.lockAvailable(false);
        Client client(port, process);
        client.send("GET /health/ready HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        const auto response = client.receive();
        BOOST_CHECK(response.find("HTTP/1.1 200 OK\r\n") == 0);
        Client unauthorized(port, process);
        const std::string json = R"({"space_id":"00000000-0000-4000-8000-000000000001","parent_id":"00000000-0000-4000-8000-000000000001","name":"denied"})";
        unauthorized.send("POST /api/v1/nodes HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Type: application/json\r\nAuthorization: Bearer " +
            std::string(43, 'A') + "\r\nContent-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json);
        BOOST_CHECK(unauthorized.receive().find("HTTP/1.1 401 Unauthorized\r\n") == 0);
        process.stop(signal);
        BOOST_CHECK(process.text.find("ANOF_STOPPED") != std::string::npos);
        fixture.lockAvailable(true);
    }
}

BOOST_AUTO_TEST_CASE(second_process_is_rejected_without_disturbing_live_instance) {
    Fixture fixture;
    Process first(fixture.arguments()); const auto port = first.ready();
    Process second(fixture.arguments()); second.exited(3);
    BOOST_CHECK(second.text.find("stage=prepare code=CONFLICT") != std::string::npos);
    BOOST_CHECK(second.text.find("ANOF_READY") == std::string::npos);
    Client client(port, first); client.send("GET /health/live HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    BOOST_CHECK(client.receive().find("HTTP/1.1 200 OK\r\n") == 0);
    first.stop(SIGTERM); fixture.lockAvailable(true);
}

BOOST_AUTO_TEST_CASE(preparation_failure_releases_lock_and_does_not_create_missing_metadata) {
    Fixture fixture;
    fixture.write("config.toml", "development_http=true\nsecret_unknown_key='DO_NOT_LOG_THIS'\n");
    Process invalid(fixture.arguments()); invalid.exited(3);
    BOOST_CHECK(invalid.text.find("INVALID_ARGUMENT") != std::string::npos);
    BOOST_CHECK(invalid.text.find("DO_NOT_LOG_THIS") == std::string::npos);
    fixture.lockAvailable(true);
    fixture.write("config.toml", "development_http=true\nport=0\n");
    fs::remove(fixture.root / ".anof/metadata.sqlite3");
    Process missing(fixture.arguments()); missing.exited(3);
    BOOST_CHECK(missing.text.find("NOT_FOUND") != std::string::npos);
    BOOST_CHECK(!fs::exists(fixture.root / ".anof/metadata.sqlite3"));
    fixture.lockAvailable(true);
}

BOOST_AUTO_TEST_CASE(signal_waits_for_active_http_cleanup_before_unlocking) {
    Fixture fixture;
    fixture.write("config.toml", "development_http=true\nport=0\nshutdown_grace_seconds=1\n");
    Process process(fixture.arguments()); const auto port = process.ready();
    Client partial(port, process);
    partial.send("POST /api/v1/nodes HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " + std::string(43, 'A') +
                 "\r\nContent-Type: application/json\r\nContent-Length: 100\r\nExpect: 100-continue\r\n\r\n");
    BOOST_CHECK(partial.receive(true).find("100 Continue") != std::string::npos);
    BOOST_REQUIRE_EQUAL(::kill(process.pid, SIGTERM), 0);
    BOOST_REQUIRE(process.wait([&] { return process.text.find("ANOF_STOPPING") != std::string::npos; }));
    fixture.lockAvailable(false);
    process.exited(0);
    (void)partial.receive();
    fixture.lockAvailable(true);
    BOOST_CHECK(process.text.find("ANOF_STOPPED") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(current_library_bind_failure_is_fatal_but_os_releases_instance_lock) {
    Fixture fixture;
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    BOOST_REQUIRE(listener >= 0);
    struct Close { int fd; ~Close() { ::close(fd); } } close{listener};
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOST_REQUIRE_EQUAL(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
    BOOST_REQUIRE_EQUAL(::listen(listener, 1), 0);
    socklen_t length = sizeof address;
    BOOST_REQUIRE_EQUAL(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    fixture.write("config.toml", "development_http=true\nport=" + std::to_string(ntohs(address.sin_port)) + "\n");
    Process process(fixture.arguments());
    BOOST_REQUIRE(process.wait([&] { return process.status.has_value(); }));
    BOOST_REQUIRE(WIFSIGNALED(*process.status));
    BOOST_CHECK_EQUAL(WTERMSIG(*process.status), SIGABRT);
    BOOST_CHECK(process.text.find("ANOF_READY") == std::string::npos);
    BOOST_CHECK(process.text.find("ANOF_STOPPED") == std::string::npos);
    fixture.lockAvailable(true);
}
}  // namespace
