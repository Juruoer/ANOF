#define BOOST_TEST_MODULE LogSink
#include <anof/runtime/LogSink.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
using namespace anof::runtime;
namespace {
struct Pipe {
    int fd[2];
    Pipe() { BOOST_REQUIRE_EQUAL(pipe2(fd, O_NONBLOCK | O_CLOEXEC), 0); }
    ~Pipe() {
        close(fd[0]);
        close(fd[1]);
    }
};
std::string readAll(int fd) {
    std::string s;
    char b[4096];
    for (;;) {
        auto n = read(fd, b, sizeof b);
        if (n <= 0) break;
        s.append(b, static_cast<std::size_t>(n));
    }
    return s;
}
}  // namespace
BOOST_AUTO_TEST_CASE(structured_record_and_secret_bearing_invalid_fields_are_not_emitted) {
    Pipe out, err;
    LogSink sink(out.fd[1], err.fd[1]);
    LogRecord r;
    r.event = LogEvent::Ready;
    r.port = 43210;
    r.route = "/private?secret=should-not-appear";
    r.code = "bad\nsecret";
    BOOST_REQUIRE(sink.tryEmit(r));
    sink.stop();
    auto text = readAll(out.fd[0]);
    BOOST_CHECK(text.find("secret") == std::string::npos);
    auto j = nlohmann::json::parse(text);
    BOOST_CHECK_EQUAL(j.at("port"), 43210);
    BOOST_CHECK_EQUAL(j.at("route"), "unmatched");
    BOOST_CHECK_EQUAL(sink.snapshot().written, 1);
}
BOOST_AUTO_TEST_CASE(full_pipe_has_bounded_queue_and_shutdown_without_detached_writer) {
    Pipe out, err;
    char bytes[4096]{};
    while (write(out.fd[1], bytes, sizeof bytes) > 0) {
    }
    LogSink sink(out.fd[1], err.fd[1]);
    LogRecord r;
    r.event = LogEvent::Access;
    for (unsigned i = 0; i < 1200; ++i) sink.tryEmit(r);
    auto before = std::chrono::steady_clock::now();
    sink.stop();
    auto elapsed = std::chrono::steady_clock::now() - before;
    BOOST_CHECK(elapsed < std::chrono::seconds(3));
    auto s = sink.snapshot();
    BOOST_CHECK(s.accepted <= 1025);
    BOOST_CHECK(s.dropped >= 175);
    BOOST_CHECK(s.writeFailures > 0);
    BOOST_CHECK(!readAll(err.fd[0]).empty());
}

BOOST_AUTO_TEST_CASE(broken_output_pipe_reports_failure_without_process_sigpipe) {
    Pipe out, err;
    ::close(out.fd[0]);
    out.fd[0] = -1;
    LogSink sink(out.fd[1], err.fd[1]);
    LogRecord r;
    r.event = LogEvent::Failure;
    BOOST_REQUIRE(sink.tryEmit(r));
    sink.stop();
    BOOST_CHECK(sink.snapshot().writeFailures > 0);
    BOOST_CHECK(!readAll(err.fd[0]).empty());
}
