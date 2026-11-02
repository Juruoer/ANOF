#include <tests/support/EventLoopWait.h>
#define BOOST_TEST_MODULE CreateDirectoryHttp
#include <anof/transport/http/CreateDirectoryHandler.h>
#include <muduox/net/EventLoop.h>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "HttpTestSupport.h"

using namespace anof;
using namespace anof::http;
using namespace anof::http::testing;
using namespace muduox::net;
namespace {
const std::string space = "10000000-0000-4000-8000-000000000001";
const std::string parent = "20000000-0000-4000-8000-000000000001";
const std::string node = "30000000-0000-4000-8000-000000000001";
std::string body() { return nlohmann::json{ { "space_id", space }, { "parent_id", parent }, { "name", "家庭照片" } }.dump(); }
std::string head(std::size_t length, bool close = true) {
    return "POST /api/v1/nodes HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer test-token\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(length) + (close ? "\r\nConnection: close" : "") + "\r\n\r\n";
}
nlohmann::json json(const std::string& wire) { return nlohmann::json::parse(wire.substr(wire.find("\r\n\r\n") + 4)); }
void status(const std::string& wire, std::string_view text) { BOOST_CHECK(wire.find("HTTP/1.1 " + std::string(text) + "\r\n") == 0); }

class Dispatcher final : public CreateDirectoryDispatcher {
public:
    enum class Phase { Queued,
                       Running,
                       Cancelled,
                       Terminal };
    struct Job {
        std::uint64_t id;
        RequestContext context;
        CreateDirectoryCommand command;
        CreateDirectoryCallback callback;
        Phase phase = Phase::Queued;
        bool scheduled = false;
    };
    class Handle final : public TaskHandle {
    public:
        Handle(std::shared_ptr<Job> job, int& cancels) : job_(std::move(job)), cancels_(cancels) {}
        CancelBeforeStartResult cancelBeforeStart() noexcept override {
            ++cancels_;
            if (job_->phase == Phase::Queued) {
                job_->phase = Phase::Cancelled;
                return CancelBeforeStartResult::Cancelled;
            }
            return job_->phase == Phase::Running ? CancelBeforeStartResult::AlreadyRunning : CancelBeforeStartResult::AlreadyTerminal;
        }
        std::uint64_t id() const noexcept override { return job_->id; }

    private:
        std::shared_ptr<Job> job_;
        int& cancels_;
    };
    EventLoop& loop;
    bool deferred = false;
    bool throwOnSubmit = false;
    bool invalidResult = false;
    std::optional<SubmitError> rejection;
    std::optional<ErrorCode> error;
    int submissions = 0;
    int deliveries = 0;
    int cancels = 0;
    std::vector<std::shared_ptr<Job>> jobs;
    explicit Dispatcher(EventLoop& value) : loop(value) {}
    TaskSubmission trySubmit(RequestContext context, CreateDirectoryCommand command, CreateDirectoryCallback callback) override {
        loop.assertInLoopThread();
        ++submissions;
        if (throwOnSubmit) throw std::runtime_error("injected submit failure");
        if (rejection) return *rejection;
        auto job = std::make_shared<Job>(Job{ static_cast<std::uint64_t>(jobs.size() + 1), context, std::move(command), std::move(callback) });
        jobs.push_back(job);
        if (!deferred) release(job);
        return AcceptedTask{ std::make_unique<Handle>(job, cancels) };
    }
    void release(const std::shared_ptr<Job>& job) {
        if (job->scheduled) return;
        job->scheduled = true;
        loop.queueInLoop([this, job] {
            auto callback = std::move(job->callback);
            const bool cancelled = job->phase == Phase::Cancelled;
            job->phase = Phase::Terminal;
            CreateDirectoryCompletion completion{ job->id, CancelledBeforeStart{}, std::nullopt };
            if (!cancelled) {
                completion.attempt = CreateDirectoryAttempt{ NodeId::parse(node).value(), job->context.requestId };
                if (error)
                    completion.outcome = Result<CreateDirectoryResult>(*error);
                else
                    completion.outcome = Result<CreateDirectoryResult>(CreateDirectoryResult{
                        { NodeId::parse(node).value(), job->command.spaceId, job->command.parentId, job->command.name,
                          UserId::parse("40000000-0000-4000-8000-000000000001").value(), Revision::from(1).value(), UtcTime{}, UtcTime{} },
                        Revision::from(2).value() });
            }
            if (invalidResult && !cancelled && !error) {
                std::get<Result<CreateDirectoryResult>>(completion.outcome).value().directory.parentId.reset();
            }
            callback(std::move(completion));
            ++deliveries;
        });
    }
};
class Fixture {
public:
    EventLoop loop;
    Codec codec;
    Identity identity;
    UnexpectedIdentityDispatcher identityDispatcher;
    bool ready = true;
    bool throwReady = false;
    bool throwTaskObserver = false;
    bool throwHttpObserver = false;
    std::vector<std::weak_ptr<HttpResponseWriter>> writers;
    Dispatcher dispatcher{ loop };
    std::vector<DirectoryTaskRecord> tasks;
    std::vector<HttpTraceRecord> exchanges;
    CreateDirectoryHandler handler{ dispatcher, [this] {
        if (throwReady) throw std::runtime_error("injected ready failure");
        return ready; }, [this](auto record) {
        tasks.push_back(std::move(record));
        if (throwTaskObserver) throw std::runtime_error("injected task observer failure"); } };
    UnexpectedTransferDispatcher uploadDispatcher;
    UnexpectedDriveDispatcher driveDispatcher;
    HttpApi api;
    HttpServer server{ &loop, InetAddress(0, true), "directory-test" };
    bool stopping = false;
    bool httpStopped = false;
    explicit Fixture(HttpApiOptions options = {}, bool failHeaders = false)
        : api(codec, identity, [this] { return ready; }, [this, stream = handler.stream()](auto context, const auto& request, const auto& writer) {
                  writers.push_back(writer);
                  return stream(context, request, writer); }, identityDispatcher, uploadDispatcher, driveDispatcher, developmentConfiguration(), { {}, [this](auto record) {
                  exchanges.push_back(std::move(record));
                  if (throwHttpObserver) throw std::runtime_error("injected HTTP observer failure"); } }, options) {
        api.configure(server);
        if (failHeaders) {
            server.setResponseHeaderPolicy([](const auto&, auto, auto, auto&) { throw std::runtime_error("injected header failure"); });
        }
        server.start();
    }
    void stop() {
        if (stopping) return;
        stopping = true;
        server.stop(0, [this] { httpStopped = true; });
    }
    ~Fixture() {
        stop();
        for (const auto& job : dispatcher.jobs) dispatcher.release(job);
        if (!anof::test::waitFor(loop, [&] { return httpStopped && dispatcher.deliveries == static_cast<int>(dispatcher.jobs.size()); }, 3)) std::terminate();
    }
    void until(const std::function<bool()>& done,
               const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        BOOST_REQUIRE(anof::test::waitFor(loop, done, 3, file, line));
    }
    std::string exchange(std::string payload) {
        Client client(server.listenAddress());
        client.send(payload);
        until([&] { client.read(); return client.eof; });
        return client.bytes;
    }
};
}  // namespace

BOOST_AUTO_TEST_CASE(createdResponseComesFromDeferredCompletion) {
    Fixture fixture;
    fixture.dispatcher.deferred = true;
    Client client(fixture.server.listenAddress());
    client.send(head(body().size()) + body());
    fixture.until([&] { return fixture.dispatcher.submissions == 1; });
    client.read();
    BOOST_CHECK(client.bytes.empty());
    BOOST_CHECK(fixture.exchanges.empty());
    const auto job = fixture.dispatcher.jobs.front();
    BOOST_CHECK_EQUAL(job->command.name, "家庭照片");
    fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "201 Created");
    const auto result = json(client.bytes);
    BOOST_CHECK_EQUAL(result.size(), 8);
    BOOST_CHECK_EQUAL(result["id"].get<std::string>(), node);
    BOOST_CHECK(client.bytes.find("location: /api/v1/nodes/" + node) != std::string::npos);
    BOOST_CHECK(client.bytes.find("etag: \"node-" + node + "-1\"") != std::string::npos);
    BOOST_CHECK(client.bytes.find("x-request-id: " + job->context.requestId.toString()) != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 1);
    BOOST_REQUIRE_EQUAL(fixture.tasks.size(), 1);
    BOOST_CHECK(!fixture.tasks[0].error);
    BOOST_CHECK(fixture.tasks[0].attempt->requestId == job->context.requestId);
}
BOOST_AUTO_TEST_CASE(fragmentedExpectRequestSubmitsOnlyAfterComplete) {
    Fixture fixture;
    Client client(fixture.server.listenAddress());
    auto headers = head(body().size());
    headers.insert(headers.size() - 2, "Expect: 100-continue\r\n");
    client.send(headers);
    fixture.until([&] { client.read(); return client.bytes.find("100 Continue") != std::string::npos; });
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 0);
    client.send(body().substr(0, 7));
    // 下一段在独立 loop 回调交付，避免用 sleep 猜测接收进度。
    fixture.loop.queueInLoop([&] { BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 0); client.send(body().substr(7)); });
    fixture.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(client.bytes.find("HTTP/1.1 201 Created") != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 1);
}
BOOST_AUTO_TEST_CASE(chunkedAndActualTrailerHandling) {
    Fixture fixture;
    const std::string headers = "POST /api/v1/nodes HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer test-token\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    char length[32];
    std::snprintf(length, sizeof length, "%zx", body().size());
    const auto chunks = std::string(length) + "\r\n" + body() + "\r\n0\r\n";
    status(fixture.exchange(headers + chunks + "\r\n"), "201 Created");
    const auto rejected = fixture.exchange(headers + chunks + "X-Extra: trailer\r\n\r\n");
    status(rejected, "400 Bad Request");
    BOOST_CHECK_EQUAL(json(rejected)["error"]["code"].get<std::string>(), "INVALID_ARGUMENT");
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 1);
}
BOOST_AUTO_TEST_CASE(invalidJsonNeverSubmits) {
    Fixture fixture;
    for (const auto& payload : { std::string{}, std::string("{}"), std::string("[]"), body().substr(0, body().size() - 1) + ",\"name\":\"again\"}" }) {
        status(fixture.exchange(head(payload.size()) + payload), "400 Bad Request");
    }
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 0);
}
BOOST_AUTO_TEST_CASE(submissionRejectionsHaveNoCompletion) {
    Fixture fixture;
    for (auto error : { SubmitError::QueueFull, SubmitError::Stopping, SubmitError::InternalError, SubmitError::RateLimited }) {
        fixture.dispatcher.rejection = error;
        const auto wire = fixture.exchange(head(body().size()) + body());
        status(wire, error == SubmitError::InternalError ? "500 Internal Server Error" : error == SubmitError::RateLimited ? "429 Too Many Requests"
                                                                                                                           : "503 Service Unavailable");
    }
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);
    BOOST_CHECK(fixture.tasks.empty());
}
BOOST_AUTO_TEST_CASE(applicationFailuresRemainDistinctOnWire) {
    Fixture fixture;
    const std::tuple<ErrorCode, const char*, const char*> cases[] = {
        { ErrorCode::Unauthenticated, "401 Unauthorized", "UNAUTHENTICATED" },
        { ErrorCode::StorageFull, "507 Insufficient Storage", "STORAGE_FULL" },
        { ErrorCode::Retryable, "503 Service Unavailable", "TEMPORARILY_UNAVAILABLE" },
        { ErrorCode::CommitOutcomeUnknown, "500 Internal Server Error", "COMMIT_OUTCOME_UNKNOWN" },
        { ErrorCode::NameConflict, "409 Conflict", "NAME_CONFLICT" },
        { ErrorCode::QuotaExceeded, "403 Forbidden", "QUOTA_EXCEEDED" }
    };
    for (const auto& [error, line, code] : cases) {
        fixture.dispatcher.error = error;
        const auto wire = fixture.exchange(head(body().size()) + body());
        status(wire, line);
        BOOST_CHECK_EQUAL(json(wire)["error"]["code"].get<std::string>(), code);
        BOOST_CHECK_EQUAL(json(wire)["error"]["request_id"].get<std::string>(), fixture.tasks.back().requestId.toString());
    }
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 6);
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 6);
}
BOOST_AUTO_TEST_CASE(stoppingAfterHeadersPreventsSubmission) {
    Fixture fixture;
    Client client(fixture.server.listenAddress());
    auto headers = head(body().size());
    headers.insert(headers.size() - 2, "Expect: 100-continue\r\n");
    client.send(headers);
    fixture.until([&] { client.read(); return !client.bytes.empty(); });
    fixture.ready = false;
    client.bytes.clear();
    client.send(body());
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "503 Service Unavailable");
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 0);
}
BOOST_AUTO_TEST_CASE(httpCancellationConsumesQueuedAndRunningLateResults) {
    for (bool running : { false, true }) {
        Fixture fixture;
        fixture.dispatcher.deferred = true;
        Client client(fixture.server.listenAddress());
        client.send(head(body().size()) + body());
        fixture.until([&] { return fixture.dispatcher.submissions == 1; });
        const auto job = fixture.dispatcher.jobs.front();
        if (running) job->phase = Dispatcher::Phase::Running;
        // 等待结果期间框架暂停读取，FIN 不保证立即可见；用真实 stop 截止触发取消。
        fixture.stop();
        fixture.until([&] { return fixture.httpStopped && fixture.dispatcher.cancels == 1; });
        BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);
        fixture.dispatcher.release(job);
        fixture.until([&] { return fixture.dispatcher.deliveries == 1; });
        BOOST_REQUIRE_EQUAL(fixture.tasks.size(), 1);
        BOOST_CHECK(!fixture.tasks[0].exchangeActive);
        BOOST_CHECK_EQUAL(fixture.tasks[0].cancelledBeforeStart, !running);
        BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 1);
    }
}
BOOST_AUTO_TEST_CASE(cancelledBeforeStartWhileHttpActiveReturns503) {
    Fixture fixture;
    fixture.dispatcher.deferred = true;
    Client client(fixture.server.listenAddress());
    client.send(head(body().size()) + body());
    fixture.until([&] { return fixture.dispatcher.submissions == 1; });
    auto job = fixture.dispatcher.jobs.front();
    job->phase = Dispatcher::Phase::Cancelled;
    fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "503 Service Unavailable");
    BOOST_CHECK(fixture.tasks[0].cancelledBeforeStart);
}
BOOST_AUTO_TEST_CASE(keepAliveAfterCreateUsesFreshExchange) {
    Fixture fixture;
    const auto wire = fixture.exchange(head(body().size(), false) + body() + "GET /health/live HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    status(wire, "201 Created");
    BOOST_CHECK(wire.find("HTTP/1.1 200 OK") != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 1);
    BOOST_REQUIRE_EQUAL(fixture.exchanges.size(), 2);
    BOOST_CHECK(fixture.exchanges[0].requestId != fixture.exchanges[1].requestId);
}

BOOST_AUTO_TEST_CASE(disconnectDuringBodyDoesNotSubmit) {
    Fixture fixture;
    Client client(fixture.server.listenAddress());
    auto headers = head(body().size());
    headers.insert(headers.size() - 2, "Expect: 100-continue\r\n");
    client.send(headers);
    fixture.until([&] { client.read(); return client.bytes.find("100 Continue") != std::string::npos; });
    client.send(body().substr(0, 7));
    ::shutdown(client.fd, SHUT_RDWR);
    fixture.until([&] { return !fixture.exchanges.empty(); });
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 0);
    BOOST_CHECK(fixture.tasks.empty());
}

BOOST_AUTO_TEST_CASE(bodyTimeoutReleasesReceivingStateWithoutSubmission) {
    Fixture fixture({ 128, 0.1, 30 });
    Client client(fixture.server.listenAddress());
    client.send(head(body().size()) + body().substr(0, 1));
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "408 Request Timeout");
    BOOST_REQUIRE_EQUAL(fixture.writers.size(), 1);
    BOOST_CHECK(fixture.writers[0].expired());
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 0);
    BOOST_REQUIRE_EQUAL(fixture.exchanges.size(), 1);
    BOOST_CHECK(fixture.exchanges[0].reason == HttpCancelReason::RequestTimeout);
}
BOOST_AUTO_TEST_CASE(responseTimeoutRetainsTaskUntilLateResultConsumption) {
    for (auto phase : { Dispatcher::Phase::Queued, Dispatcher::Phase::Running, Dispatcher::Phase::Terminal }) {
        Fixture fixture({ 1, 15, 0.1 });
        fixture.dispatcher.deferred = true;
        Client first(fixture.server.listenAddress());
        first.send(head(body().size(), false) + body());
        fixture.until([&] { return fixture.dispatcher.submissions == 1; });
        fixture.dispatcher.jobs[0]->phase = phase;
        fixture.until([&] { first.read(); return first.eof; });
        status(first.bytes, "500 Internal Server Error");
        BOOST_CHECK(first.bytes.find("504") == std::string::npos);
        BOOST_CHECK_EQUAL(fixture.dispatcher.cancels, 1);
        BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);
        BOOST_REQUIRE_EQUAL(fixture.writers.size(), 1);
        BOOST_CHECK(fixture.writers[0].expired());
        BOOST_CHECK(fixture.exchanges[0].reason == HttpCancelReason::ResponseTimeout);
        // HTTP 名额已经归还，但第一个任务的完成仍未消费；后续请求用独立 writer。
        const auto next = fixture.exchange("GET /health/live HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        status(next, "200 OK");
        fixture.dispatcher.release(fixture.dispatcher.jobs[0]);
        fixture.until([&] { return fixture.dispatcher.deliveries == 1; });
        BOOST_REQUIRE_EQUAL(fixture.tasks.size(), 1);
        BOOST_CHECK(!fixture.tasks[0].exchangeActive);
        BOOST_CHECK_EQUAL(fixture.tasks[0].cancelledBeforeStart, phase == Dispatcher::Phase::Queued);
        BOOST_CHECK_EQUAL(fixture.exchanges.size(), 2);
    }
}
BOOST_AUTO_TEST_CASE(normalAndRejectedExchangesReleaseWriterOwnership) {
    Fixture fixture;
    status(fixture.exchange(head(body().size()) + body()), "201 Created");
    BOOST_REQUIRE_EQUAL(fixture.writers.size(), 1);
    BOOST_CHECK(fixture.writers.back().expired());
    BOOST_CHECK_EQUAL(fixture.dispatcher.cancels, 0);
    fixture.dispatcher.rejection = SubmitError::QueueFull;
    status(fixture.exchange(head(body().size()) + body()), "503 Service Unavailable");
    BOOST_CHECK(fixture.writers.back().expired());
    status(fixture.exchange(head(2) + "{}"), "400 Bad Request");
    BOOST_CHECK(fixture.writers.back().expired());
}
BOOST_AUTO_TEST_CASE(observerExceptionsDoNotChangeResponseOrCompletion) {
    Fixture fixture;
    fixture.throwTaskObserver = fixture.throwHttpObserver = true;
    for (int i = 0; i < 2; ++i) {
        status(fixture.exchange(head(body().size()) + body()), "201 Created");
        BOOST_CHECK(fixture.writers.back().expired());
    }
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 2);
    BOOST_CHECK_EQUAL(fixture.tasks.size(), 2);
    BOOST_CHECK_EQUAL(fixture.exchanges.size(), 2);
}
BOOST_AUTO_TEST_CASE(invalidApplicationResultIsNotSuccessOrRetry) {
    Fixture fixture;
    fixture.dispatcher.invalidResult = true;
    const auto response = fixture.exchange(head(body().size()) + body());
    status(response, "500 Internal Server Error");
    BOOST_CHECK_EQUAL(json(response)["error"]["code"].get<std::string>(), "INTERNAL_ERROR");
    BOOST_CHECK(response.find("201 Created") == std::string::npos);
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 1);
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 1);
    BOOST_CHECK(fixture.writers.back().expired());
}
BOOST_AUTO_TEST_CASE(headerPreparationFailureHasOnlyOneFrameworkResponse) {
    Fixture fixture({}, true);
    const auto response = fixture.exchange(head(body().size()) + body());
    status(response, "500 Internal Server Error");
    BOOST_CHECK(response.find("HTTP/1.1", 1) == std::string::npos);
    BOOST_CHECK(response.find("201 Created") == std::string::npos);
    BOOST_REQUIRE_EQUAL(fixture.exchanges.size(), 1);
    BOOST_CHECK(fixture.exchanges[0].outcome == HttpExchangeOutcome::Failed);
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 1);
    BOOST_CHECK(fixture.writers.back().expired());
}
BOOST_AUTO_TEST_CASE(synchronousDependencyExceptionsEndExchangeWithoutRetry) {
    for (bool submit : { false, true }) {
        Fixture fixture;
        if (submit)
            fixture.dispatcher.throwOnSubmit = true;
        else
            fixture.throwReady = true;
        const auto response = fixture.exchange(head(body().size()) + body());
        status(response, "500 Internal Server Error");
        BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, submit ? 1 : 0);
        BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);
        BOOST_CHECK(fixture.writers.back().expired());
        BOOST_CHECK_EQUAL(fixture.exchanges.size(), 1);
    }
}
BOOST_AUTO_TEST_CASE(repeatedStopDoesNotRetirePendingCompletion) {
    Fixture fixture;
    fixture.dispatcher.deferred = true;
    Client client(fixture.server.listenAddress());
    client.send(head(body().size()) + body());
    fixture.until([&] { return fixture.dispatcher.submissions == 1; });
    fixture.dispatcher.jobs[0]->phase = Dispatcher::Phase::Running;
    fixture.stop();
    BOOST_CHECK(!fixture.server.stop(30, [] { BOOST_FAIL("second stop callback must not run"); }));
    fixture.until([&] { return fixture.httpStopped; });
    BOOST_CHECK(fixture.writers.back().expired());
    BOOST_CHECK_EQUAL(fixture.dispatcher.cancels, 1);
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);
    fixture.dispatcher.release(fixture.dispatcher.jobs[0]);
    fixture.until([&] { return fixture.dispatcher.deliveries == 1; });
    BOOST_CHECK(!fixture.tasks[0].cancelledBeforeStart);
    BOOST_CHECK(!fixture.tasks[0].exchangeActive);
}

BOOST_AUTO_TEST_CASE(gracefulStopRejectsCompletionOfPreviouslyAcceptedBody) {
    Fixture fixture;
    Client client(fixture.server.listenAddress());
    auto headers = head(body().size());
    headers.insert(headers.size() - 2, "Expect: 100-continue\r\n");
    client.send(headers);
    fixture.until([&] { client.read(); return client.bytes.find("100 Continue") != std::string::npos; });
    fixture.ready = false;
    fixture.stopping = true;
    BOOST_REQUIRE(fixture.server.stop(1, [&] { fixture.httpStopped = true; }));
    client.bytes.clear();
    client.send(body());
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "503 Service Unavailable");
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 0);
    BOOST_CHECK(fixture.writers.back().expired());
}
BOOST_AUTO_TEST_CASE(lateUnknownCommitRemainsDiagnosticAfterTimeout) {
    Fixture fixture({ 128, 15, 0.1 });
    fixture.dispatcher.deferred = true;
    fixture.dispatcher.error = ErrorCode::CommitOutcomeUnknown;
    Client client(fixture.server.listenAddress());
    client.send(head(body().size()) + body());
    fixture.until([&] { return fixture.dispatcher.submissions == 1; });
    fixture.dispatcher.jobs[0]->phase = Dispatcher::Phase::Running;
    fixture.until([&] { client.read(); return client.eof; });
    const auto timedOut = client.bytes;
    fixture.dispatcher.release(fixture.dispatcher.jobs[0]);
    fixture.until([&] { return fixture.dispatcher.deliveries == 1; });
    BOOST_CHECK(fixture.tasks[0].error == ErrorCode::CommitOutcomeUnknown);
    BOOST_CHECK(!fixture.tasks[0].exchangeActive);
    BOOST_REQUIRE(fixture.tasks[0].attempt);
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 1);
    client.read();
    BOOST_CHECK_EQUAL(client.bytes, timedOut);
    BOOST_CHECK_EQUAL(fixture.exchanges.size(), 1);
}
