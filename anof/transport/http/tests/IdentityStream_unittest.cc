#include <tests/support/EventLoopWait.h>
#define BOOST_TEST_MODULE IdentityStream
#include <anof/transport/http/AccountResponse.h>
#include <anof/transport/http/IdentityResponse.h>
#include <anof/transport/http/IdentityStream.h>
#include <anof/transport/http/internal/ExchangeBuffer.h>
#include <muduox/net/EventLoop.h>

#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "HttpTestSupport.h"
#include "IdentityDispatcherFixture.h"

using namespace anof;
using namespace anof::http;
using namespace anof::http::testing;
using namespace muduox::net;
namespace {
using Fake = IdentityDispatcherFixture;
using ReadJob = Fake::TypedJob<GetMeCommand, CurrentUserView>;
using LoginJob = Fake::TypedJob<LoginCommand, LoginResult, RequestId>;
using LogoutJob = Fake::TypedJob<LogoutCommand, void>;
UserProfile user() {
    return { UserId::parse("40000000-0000-4000-8000-000000000001").value(), LoginName::parse("alice").value(), DisplayName::parse("家庭").value(), UserRole::Member, true, AuthEpoch::from(1).value(), Revision::from(1).value(), UtcTime{}, UtcTime{} };
}
std::string payload() { return R"({"login":"Alice","password":"example-only-password","credential_mode":"bearer"})"; }
std::string request(std::string body = {}, bool close = true) {
    return "POST /test HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + (close ? "\r\nConnection: close" : "") + "\r\n\r\n" + body;
}
void status(const std::string& wire, const char* expected) { BOOST_CHECK(wire.find(std::string("HTTP/1.1 ") + expected + "\r\n") == 0); }
class Fixture {
public:
    enum class Mode { Read,
                      Login,
                      Logout };
    EventLoop loop;
    Identity identity;
    Fake dispatcher{ loop };
    Mode mode;
    bool ready = true;
    ProtocolError (*mapError)(ErrorCode) = protocolError;
    bool throwParse = false;
    bool throwReady = false;
    std::string_view observedBody;
    bool throwFormat = false;
    bool invalidFormat = false;
    bool oversizedFormat = false;
    bool duplicateComplete = false;
    std::size_t formatCalls = 0;
    std::size_t largeBytes = 0;
    std::vector<std::weak_ptr<HttpResponseWriter>> writers;
    HttpServer server{ &loop, InetAddress(0, true), "identity-stream-test" };
    bool stopped = false;
    explicit Fixture(Mode value = Mode::Read, double responseSeconds = 30, double bodySeconds = 15, bool failHeaders = false) : mode(value) {
        server.setHttpRequestSelector([this, responseSeconds, bodySeconds](const HttpRequest&, const HttpRequestMetadata& metadata) {
            const auto id = identity.forExchange(metadata.exchange.connection, metadata.exchange.request);
            const RequestContext context{ { SessionDigest(SessionDigest::Bytes{}), CredentialMode::Bearer }, id };
            HttpStreamCallback stream;
            if (mode == Mode::Read) {
                stream = makeIdentityStream<GetMeCommand, CurrentUserView>(dispatcher, context, RequestBody::None, [this](std::string_view) -> Parsed<GetMeCommand> {
                        if (throwParse) throw std::runtime_error("Injected parser failure");
                        return GetMeCommand{}; }, [this](CurrentUserView&& result, RequestId requestId) -> Result<JsonResponse> {
                        ++formatCalls;
                        if (throwFormat) throw std::runtime_error("Injected formatter failure");
                        if (invalidFormat) return ErrorCode::InternalError;
                        auto response = currentUserResponse(result, requestId);
                        if (largeBytes || oversizedFormat) response.value().body = nlohmann::json{{"data", std::string(oversizedFormat ? kMaxIdentityResponseBytes : largeBytes, 's')}}.dump();
                        return response; }, [this] { if (throwReady) throw std::runtime_error("Injected ready failure"); return ready; }, mapError);
            } else if (mode == Mode::Login) {
                const auto source = std::get<AuthenticationSource>(readAuthenticationSource(metadata));
                stream = makeIdentityStream<LoginCommand, LoginResult>(dispatcher, id, RequestBody::Json, [this, source](std::string_view body) { observedBody = body; return parseLoginJson(body, source); }, [this](LoginResult&& result, RequestId requestId) { ++formatCalls; return loginResponse(std::move(result), requestId); }, [this] { if (throwReady) throw std::runtime_error("Injected ready failure"); return ready; });
            } else {
                stream = makeIdentityStream<LogoutCommand, void>(dispatcher, context, RequestBody::None, [](std::string_view) -> Parsed<LogoutCommand> { return LogoutCommand{}; }, [this](RequestId requestId) -> Result<JsonResponse> { ++formatCalls; return noContentResponse(requestId); }, [this] { if (throwReady) throw std::runtime_error("Injected ready failure"); return ready; });
            }
            auto decision = HttpRequestDecision::streaming([this, stream](const auto& head, const auto& writer) {
                writers.push_back(writer);
                auto callbacks = stream(head, writer);
                if (duplicateComplete) {
                    const auto complete = callbacks.complete;
                    callbacks.complete = [complete](const auto& req) { complete(req); complete(req); };
                }
                return callbacks;
            });
            // 测试保留较大的框架 body 上限，以直接覆盖公共状态自身的 64 KiB/无正文防线。
            decision.attributes().restrictBodyLimit(128 * 1024).restrictBodyTimeout(bodySeconds).restrictResponseTimeout(responseSeconds);
            return decision;
        });
        if (failHeaders) server.setResponseHeaderPolicy([](const auto&, auto, auto, auto&) { throw std::runtime_error("Injected header policy failure"); });
        server.start();
    }
    void until(const std::function<bool()>& done,
               const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        BOOST_REQUIRE(anof::test::waitFor(loop, done, 3, file, line));
    }
    void stop() {
        server.stop(0, [this] { stopped = true; });
    }
    ~Fixture() {
        if (!stopped) stop();
        dispatcher.releaseAll();
        until([this] { return stopped && dispatcher.deliveries == dispatcher.jobs.size(); });
    }
    template <typename Job>
    std::shared_ptr<Job> job(std::size_t index = 0) {
        auto result = std::dynamic_pointer_cast<Job>(dispatcher.jobs.at(index));
        BOOST_REQUIRE(result);
        return result;
    }
    std::string exchange(const std::string& text) {
        Client client(server.listenAddress());
        client.send(text);
        until([&] { client.read(); return client.eof; });
        return client.bytes;
    }
};
}  // namespace

BOOST_AUTO_TEST_CASE(bufferClearsConsumedAndCancelledBytesWithoutReallocation) {
    internal::ExchangeBuffer buffer(12);
    BOOST_REQUIRE(buffer.append("secret-value"));
    const char* storage = buffer.view().data();
    BOOST_CHECK(!buffer.append("overflow"));
    BOOST_CHECK_EQUAL(buffer.take(6), "secret");
    for (int i = 0; i < 6; ++i) BOOST_CHECK_EQUAL(storage[i], 0);
    BOOST_CHECK_EQUAL(buffer.view(), "-value");
    buffer.clear();
    for (int i = 0; i < 12; ++i) BOOST_CHECK_EQUAL(storage[i], 0);
    BOOST_CHECK(buffer.view().empty());
    BOOST_REQUIRE(buffer.append("new"));
    BOOST_CHECK(buffer.view().data() == storage);
    BOOST_CHECK_EQUAL(buffer.take(64), "new");
    BOOST_CHECK_THROW(buffer.take(0), std::invalid_argument);
}
BOOST_AUTO_TEST_CASE(bodylessRequestSubmitsOnceAndCompletesLater) {
    Fixture fixture;
    fixture.duplicateComplete = true;
    Client client(fixture.server.listenAddress());
    client.send(request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    client.read();
    BOOST_CHECK(client.bytes.empty());
    const auto job = fixture.job<ReadJob>();
    BOOST_CHECK(job->context.credential.mode == CredentialMode::Bearer);
    job->result.emplace(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() });
    fixture.dispatcher.release(job);
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "200 OK");
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 1);
    BOOST_CHECK_EQUAL(fixture.formatCalls, 1);
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 1);
    BOOST_CHECK(!job->command);
    fixture.until([&] { return fixture.writers.front().expired(); });
}
BOOST_AUTO_TEST_CASE(jsonBodyIsOwnedAndMoveOnlyResultIsDelivered) {
    Fixture fixture(Fixture::Mode::Login);
    Client client(fixture.server.listenAddress());
    auto text = request(payload());
    const auto split = text.size() - 10;
    client.send(text.substr(0, split));
    bool gate = false;
    fixture.loop.queueInLoop([&] { gate = true; });
    fixture.until([&] { return gate; });
    BOOST_CHECK(fixture.dispatcher.jobs.empty());
    client.send(text.substr(split));
    text.assign(text.size(), 'x');
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    const auto job = fixture.job<LoginJob>();
    // 请求仍在等待任务，缓冲分配仍有效；解析后的原始密码正文已清零。
    BOOST_REQUIRE(!fixture.observedBody.empty());
    for (const char byte : fixture.observedBody) BOOST_CHECK_EQUAL(byte, 0);
    fixture.observedBody = {};
    BOOST_CHECK_EQUAL(job->command->login.value(), "alice");
    BOOST_CHECK_EQUAL(job->command->password.view(), "example-only-password");
    BOOST_CHECK(job->command->source == AuthenticationSource::ipv4({ 127, 0, 0, 1 }));
    job->result.emplace(LoginResult{ SecretToken::session(std::string(43, 'A')).value(), UtcTime{}, user() });
    fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "201 Created");
    const auto json = nlohmann::json::parse(client.bytes.substr(client.bytes.find("\r\n\r\n") + 4));
    BOOST_CHECK_EQUAL(json["access_token"].get<std::string>(), std::string(43, 'A'));
    BOOST_CHECK(!job->command && !job->result);
}
BOOST_AUTO_TEST_CASE(voidCompletionProducesEmpty204) {
    Fixture fixture(Fixture::Mode::Logout);
    Client client(fixture.server.listenAddress());
    client.send(request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<LogoutJob>();
    job->result.emplace(Result<void>{});
    fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "204 No Content");
    BOOST_CHECK(client.bytes.substr(client.bytes.find("\r\n\r\n") + 4).empty());
}
BOOST_AUTO_TEST_CASE(rejectionsAndNotReadyDoNotComplete) {
    Fixture fixture;
    for (const auto& item : { std::pair{ SubmitError::QueueFull, "503 Service Unavailable" }, std::pair{ SubmitError::Stopping, "503 Service Unavailable" }, std::pair{ SubmitError::RateLimited, "429 Too Many Requests" }, std::pair{ SubmitError::InternalError, "500 Internal Server Error" } }) {
        fixture.dispatcher.rejection = item.first;
        status(fixture.exchange(request()), item.second);
    }
    fixture.ready = false;
    status(fixture.exchange(request()), "503 Service Unavailable");
    BOOST_CHECK_EQUAL(fixture.dispatcher.submissions, 4);
    BOOST_CHECK(fixture.dispatcher.jobs.empty());
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);
}
BOOST_AUTO_TEST_CASE(invalidBodyAndTrailersNeverSubmit) {
    {
        Fixture fixture;
        status(fixture.exchange(request("x")), "400 Bad Request");
        BOOST_CHECK(fixture.dispatcher.jobs.empty());
    }
    Fixture fixture(Fixture::Mode::Login);
    status(fixture.exchange(request("{}")), "400 Bad Request");
    status(fixture.exchange(request(std::string(kMaxJsonBytes + 1, 'x'))), "413 Content Too Large");
    status(fixture.exchange("POST /test HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n2\r\n{}\r\n0\r\nX-Test: yes\r\n\r\n"), "400 Bad Request");
    BOOST_CHECK(fixture.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(largeResponseIsSentInMultipleWriterSizedChunks) {
    Fixture fixture;
    fixture.largeBytes = 180 * 1024;
    Client client(fixture.server.listenAddress());
    client.send(request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<ReadJob>();
    job->result.emplace(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() });
    fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "200 OK");
    const auto expected = nlohmann::json{ { "data", std::string(fixture.largeBytes, 's') } }.dump();
    BOOST_CHECK_EQUAL(client.bytes.substr(client.bytes.find("\r\n\r\n") + 4), expected);
    BOOST_CHECK(client.bytes.find("Content-Length: " + std::to_string(expected.size())) != std::string::npos);
    fixture.until([&] { return fixture.writers.front().expired(); });
}
BOOST_AUTO_TEST_CASE(timeoutDuringPasswordWorkCancelsBeforeFinalPhase) {
    Fixture fixture(Fixture::Mode::Login, 0.03);
    Client client(fixture.server.listenAddress());
    client.send(request(payload()));
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<LoginJob>();
    BOOST_REQUIRE(fixture.dispatcher.startPassword(job));
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "500 Internal Server Error");
    BOOST_CHECK(job->cancelled);
    BOOST_CHECK(!fixture.dispatcher.startFinal(job));
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);  // 密码阶段尚未退出。
    fixture.until([&] { return fixture.writers.front().expired(); });
    fixture.dispatcher.release(job);
    fixture.until([&] { return fixture.dispatcher.deliveries == 1; });
    BOOST_CHECK_EQUAL(fixture.formatCalls, 0);
    BOOST_CHECK(!job->command);
}
BOOST_AUTO_TEST_CASE(runningFinalTransactionSurvivesHttpStopAndLateSecretIsDiscarded) {
    Fixture fixture(Fixture::Mode::Login);
    Client client(fixture.server.listenAddress());
    client.send(request(payload()));
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<LoginJob>();
    BOOST_REQUIRE(fixture.dispatcher.startFinal(job));
    fixture.stop();
    fixture.until([&] { return fixture.stopped; });
    BOOST_CHECK(!job->cancelled);
    BOOST_CHECK_EQUAL(fixture.dispatcher.cancellations, 1);
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 0);
    fixture.until([&] { return fixture.writers.front().expired(); });
    job->result.emplace(LoginResult{ SecretToken::session(std::string(43, 'A')).value(), UtcTime{}, user() });
    fixture.dispatcher.release(job);
    fixture.until([&] { return fixture.dispatcher.deliveries == 1; });
    client.read();
    BOOST_CHECK_EQUAL(fixture.formatCalls, 0);
    BOOST_CHECK(client.bytes.find(std::string(43, 'A')) == std::string::npos);
    BOOST_CHECK(!job->command && !job->result);
}
BOOST_AUTO_TEST_CASE(queuedCancellationCompletesOnceAndCapacityWaitsForDelivery) {
    Fixture fixture;
    fixture.dispatcher.capacity = 1;
    Client first(fixture.server.listenAddress());
    first.send(request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    status(fixture.exchange(request()), "503 Service Unavailable");
    auto job = fixture.job<ReadJob>();
    job->cancelled = true;
    fixture.dispatcher.release(job);
    fixture.dispatcher.release(job);
    fixture.until([&] { first.read(); return first.eof; });
    status(first.bytes, "503 Service Unavailable");
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 1);
    BOOST_CHECK_EQUAL(fixture.formatCalls, 0);
    Client second(fixture.server.listenAddress());
    second.send(request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 2; });
    auto next = fixture.job<ReadJob>(1);
    next->result.emplace(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() });
    fixture.dispatcher.release(next);
    fixture.until([&] { second.read(); return second.eof; });
    status(second.bytes, "200 OK");
}
BOOST_AUTO_TEST_CASE(keepAliveRequestsHaveSeparateStateAndIds) {
    Fixture fixture;
    Client client(fixture.server.listenAddress());
    client.send(request({}, false) + request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto first = fixture.job<ReadJob>();
    first->result.emplace(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() });
    fixture.dispatcher.release(first);
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 2; });
    auto second = fixture.job<ReadJob>(1);
    second->result.emplace(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() });
    fixture.dispatcher.release(second);
    fixture.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(first->context.requestId != second->context.requestId);
    BOOST_CHECK(client.bytes.find(first->context.requestId.toString()) != std::string::npos);
    BOOST_CHECK(client.bytes.find(second->context.requestId.toString()) != std::string::npos);
    BOOST_CHECK_EQUAL(fixture.formatCalls, 2);
}
BOOST_AUTO_TEST_CASE(errorResultsNeverInvokeSuccessFormatter) {
    Fixture fixture;
    for (const auto& item : { std::pair{ ErrorCode::RateLimited, "429 Too Many Requests" }, std::pair{ ErrorCode::SessionLimitReached, "409 Conflict" }, std::pair{ ErrorCode::CommitOutcomeUnknown, "500 Internal Server Error" }, std::pair{ ErrorCode::Unauthenticated, "401 Unauthorized" } }) {
        Client client(fixture.server.listenAddress());
        client.send(request());
        const auto count = fixture.dispatcher.jobs.size();
        fixture.until([&] { return fixture.dispatcher.jobs.size() == count + 1; });
        auto job = fixture.job<ReadJob>(count);
        job->result.emplace(item.first);
        fixture.dispatcher.release(job);
        fixture.until([&] { client.read(); return client.eof; });
        status(client.bytes, item.second);
    }
    BOOST_CHECK_EQUAL(fixture.formatCalls, 0);
}
BOOST_AUTO_TEST_CASE(formattingFailureAndOversizeTerminateWithoutSuccess) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture fixture;
        fixture.throwFormat = mode == 0;
        fixture.invalidFormat = mode == 1;
        fixture.oversizedFormat = mode == 2;
        Client client(fixture.server.listenAddress());
        client.send(request());
        fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
        auto job = fixture.job<ReadJob>();
        job->result.emplace(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() });
        fixture.dispatcher.release(job);
        fixture.until([&] { client.read(); return client.eof; });
        if (mode == 1) status(client.bytes, "500 Internal Server Error");
        BOOST_CHECK(client.bytes.find("200 OK") == std::string::npos);
        fixture.until([&] { return fixture.writers.front().expired(); });
    }
}
BOOST_AUTO_TEST_CASE(bodyTimeoutAndHeaderPolicyFailureReleaseState) {
    {
        Fixture fixture(Fixture::Mode::Login, 30, 0.02);
        Client client(fixture.server.listenAddress());
        client.send("POST /test HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\nx");
        fixture.until([&] { client.read(); return client.eof; });
        status(client.bytes, "408 Request Timeout");
        BOOST_CHECK(fixture.dispatcher.jobs.empty());
        fixture.until([&] { return fixture.writers.front().expired(); });
    }
    Fixture fixture(Fixture::Mode::Read, 30, 15, true);
    Client client(fixture.server.listenAddress());
    client.send(request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<ReadJob>();
    job->result.emplace(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() });
    fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(client.bytes.find("200 OK") == std::string::npos);
    fixture.until([&] { return fixture.writers.front().expired(); });
}

BOOST_AUTO_TEST_CASE(fixtureHandlesRespectPhaseCancellationAndDeferredCapacity) {
    Fixture fixture;
    auto id = RequestId::parse("50000000-0000-4000-8000-000000000001").value();
    const RequestContext context{ { SessionDigest(SessionDigest::Bytes{}), CredentialMode::Bearer }, id };
    auto parsed = parseLoginJson(payload(), AuthenticationSource::ipv4({ 127, 0, 0, 1 }));
    bool cancelled = false;
    auto submission = fixture.dispatcher.submit(id, std::get<LoginCommand>(std::move(parsed)), [&](TaskCompletion<LoginResult> value) {
        cancelled = std::holds_alternative<CancelledBeforeStart>(value.outcome);
    });
    auto handle = std::move(std::get<AcceptedTask>(submission).handle);
    auto job = fixture.job<LoginJob>();
    BOOST_REQUIRE(fixture.dispatcher.startPassword(job));
    BOOST_CHECK(handle->cancelBeforeStart() == CancelBeforeStartResult::Cancelled);
    BOOST_CHECK(handle->cancelBeforeStart() == CancelBeforeStartResult::AlreadyTerminal);
    BOOST_CHECK(!fixture.dispatcher.startFinal(job));
    BOOST_CHECK(!cancelled);
    fixture.dispatcher.release(job);
    fixture.until([&] { return fixture.dispatcher.deliveries == 1; });
    BOOST_CHECK(cancelled);
    fixture.dispatcher.capacity = 1;
    bool success = false;
    submission = fixture.dispatcher.submit(context, GetMeCommand{}, [&](TaskCompletion<CurrentUserView> value) {
        success = std::get<Result<CurrentUserView>>(value.outcome).hasValue();
    });
    handle = std::move(std::get<AcceptedTask>(submission).handle);
    auto running = fixture.job<ReadJob>(1);
    BOOST_REQUIRE(fixture.dispatcher.startFinal(running));
    BOOST_CHECK(handle->cancelBeforeStart() == CancelBeforeStartResult::AlreadyRunning);
    running->result.emplace(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() });
    fixture.dispatcher.release(running);
    BOOST_CHECK(handle->cancelBeforeStart() == CancelBeforeStartResult::AlreadyTerminal);
    auto rejected = fixture.dispatcher.submit(context, GetMeCommand{}, [](auto) { BOOST_FAIL("Rejected job completed"); });
    BOOST_REQUIRE(std::holds_alternative<SubmitError>(rejected));
    BOOST_CHECK(std::get<SubmitError>(rejected) == SubmitError::QueueFull);
    fixture.until([&] { return fixture.dispatcher.deliveries == 2; });
    BOOST_CHECK(success);
    submission = fixture.dispatcher.submit(context, GetMeCommand{}, [](auto) {});
    const auto cancels = fixture.dispatcher.cancellations;
    std::get<AcceptedTask>(submission).handle.reset();
    BOOST_CHECK_EQUAL(fixture.dispatcher.cancellations, cancels);
    BOOST_CHECK(!fixture.dispatcher.jobs.back()->cancelled);
}
BOOST_AUTO_TEST_CASE(preAdmissionExceptionsAndPartialDisconnectReleaseHttpState) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture fixture;
        fixture.throwParse = mode == 0;
        fixture.throwReady = mode == 1;
        fixture.dispatcher.throwOnSubmit = mode == 2;
        const auto wire = fixture.exchange(request());
        BOOST_CHECK(wire.find("200 OK") == std::string::npos);
        BOOST_CHECK(fixture.dispatcher.jobs.empty());
        fixture.until([&] { return fixture.writers.front().expired(); });
    }
    Fixture fixture(Fixture::Mode::Login);
    Client client(fixture.server.listenAddress());
    client.send("POST /test HTTP/1.1\r\nHost: localhost\r\nContent-Length: 100\r\n\r\npassword");
    fixture.until([&] { return !fixture.writers.empty(); });
    ::shutdown(client.fd, SHUT_RDWR);
    fixture.until([&] { return fixture.writers.front().expired(); });
    BOOST_CHECK(fixture.dispatcher.jobs.empty());
}
BOOST_AUTO_TEST_CASE(gracefulStopAllowsAcceptedCompletionAndNotifiesOnce) {
    Fixture fixture(Fixture::Mode::Login);
    Client client(fixture.server.listenAddress());
    client.send(request(payload()));
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<LoginJob>();
    BOOST_REQUIRE(fixture.dispatcher.startFinal(job));
    std::size_t notifications = 0;
    BOOST_CHECK(fixture.server.stop(0.5, [&] { ++notifications; fixture.stopped = true; }));
    BOOST_CHECK(!fixture.server.stop(0, [&] { BOOST_ERROR("Repeated stop replaced callback"); }));
    BOOST_CHECK(!fixture.stopped);
    job->result.emplace(LoginResult{ SecretToken::session(std::string(43, 'A')).value(), UtcTime{}, user() });
    fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof && fixture.stopped; });
    status(client.bytes, "201 Created");
    BOOST_CHECK_EQUAL(notifications, 1);
    BOOST_CHECK_EQUAL(fixture.formatCalls, 1);
    BOOST_CHECK_EQUAL(fixture.dispatcher.cancellations, 0);
    fixture.until([&] { return fixture.writers.front().expired(); });
}
BOOST_AUTO_TEST_CASE(readyCompletionAfterCancellationDoesNotFormatSecret) {
    Fixture fixture(Fixture::Mode::Login);
    Client client(fixture.server.listenAddress());
    client.send(request(payload()));
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<LoginJob>();
    job->result.emplace(LoginResult{ SecretToken::session(std::string(43, 'A')).value(), UtcTime{}, user() });
    fixture.dispatcher.release(job);  // 已排队，但 loop 尚未交付，取消返回 AlreadyTerminal。
    auto writer = fixture.writers.front().lock();
    BOOST_REQUIRE(writer);
    writer->abort();
    writer.reset();
    fixture.until([&] { client.read(); return client.eof && fixture.dispatcher.deliveries == 1; });
    BOOST_CHECK(!job->cancelled);
    BOOST_CHECK_EQUAL(fixture.dispatcher.cancellations, 1);
    BOOST_CHECK_EQUAL(fixture.formatCalls, 0);
    BOOST_CHECK(client.bytes.find(std::string(43, 'A')) == std::string::npos);
    BOOST_CHECK(!job->result && !job->command);
    fixture.until([&] { return fixture.writers.front().expired(); });
}
BOOST_AUTO_TEST_CASE(wrongTaskIdentityAbortsAndDiscardsEventualResult) {
    Fixture fixture;
    Client client(fixture.server.listenAddress());
    client.send(request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<ReadJob>();
    // 故障注入：dispatcher 违反任务身份契约，不能把结果送入当前交换。
    job->completion({ job->id + 1, Result<CurrentUserView>(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() }) });
    fixture.until([&] { client.read(); return client.eof; });
    BOOST_CHECK(job->cancelled);
    fixture.dispatcher.release(job);
    fixture.until([&] { return fixture.dispatcher.deliveries == 1; });
    BOOST_CHECK_EQUAL(fixture.formatCalls, 0);
    BOOST_CHECK(client.bytes.find("200 OK") == std::string::npos);
    fixture.until([&] { return fixture.writers.front().expired(); });
}
BOOST_AUTO_TEST_CASE(duplicateOldCompletionCannotWriteToNextKeepAliveExchange) {
    Fixture fixture;
    Client client(fixture.server.listenAddress());
    client.send(request({}, false) + request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto first = fixture.job<ReadJob>();
    auto duplicate = first->completion;
    first->result.emplace(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() });
    fixture.dispatcher.release(first);
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 2; });
    // 故障注入：合法完成已交付后再次调用旧闭包，下一交换必须保持等待。
    duplicate({ first->id, Result<CurrentUserView>(CurrentUserView{ user(), InstanceId::parse("70000000-0000-4000-8000-000000000001").value() }) });
    BOOST_CHECK_EQUAL(fixture.formatCalls, 1);
    auto second = fixture.job<ReadJob>(1);
    second->result.emplace(ErrorCode::Forbidden);
    fixture.dispatcher.release(second);
    fixture.until([&] { client.read(); return client.eof; });
    BOOST_CHECK_EQUAL(fixture.formatCalls, 1);
    const auto firstStatus = client.bytes.find("HTTP/1.1 200 OK");
    BOOST_CHECK_EQUAL(firstStatus, 0);
    BOOST_CHECK(client.bytes.find("HTTP/1.1 200 OK", firstStatus + 1) == std::string::npos);
    BOOST_CHECK(client.bytes.find("HTTP/1.1 403 Forbidden") != std::string::npos);
    fixture.until([&] { return fixture.writers[0].expired() && fixture.writers[1].expired(); });
}
BOOST_AUTO_TEST_CASE(operationMapperChangesOnlyAcceptedBusinessErrors) {
    Fixture fixture;
    fixture.mapError = quotaUpdateError;
    Client client(fixture.server.listenAddress());
    client.send(request());
    fixture.until([&] { return fixture.dispatcher.jobs.size() == 1; });
    auto job = fixture.job<ReadJob>();
    job->result.emplace(ErrorCode::QuotaExceeded);
    fixture.dispatcher.release(job);
    fixture.until([&] { client.read(); return client.eof; });
    status(client.bytes, "409 Conflict");
    BOOST_CHECK_EQUAL(nlohmann::json::parse(client.bytes.substr(client.bytes.find("\r\n\r\n") + 4))["error"]["code"].get<std::string>(), "QUOTA_EXCEEDED");
    BOOST_CHECK_EQUAL(fixture.formatCalls, 0);
    fixture.dispatcher.rejection = SubmitError::QueueFull;
    status(fixture.exchange(request()), "503 Service Unavailable");
    fixture.dispatcher.rejection = SubmitError::RateLimited;
    status(fixture.exchange(request()), "429 Too Many Requests");
    BOOST_CHECK_EQUAL(fixture.dispatcher.deliveries, 1);
}
