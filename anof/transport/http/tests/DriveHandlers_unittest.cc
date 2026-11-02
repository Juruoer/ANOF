#define BOOST_TEST_MODULE DriveHandlers
#include <nlohmann/json.hpp>

#include "DriveTestSupport.h"
using namespace anof;
using namespace anof::http;
using namespace anof::http::testing;
using Json = nlohmann::json;
namespace {
void status(const std::string& wire, int code) { BOOST_CHECK_MESSAGE(wire.find("HTTP/1.1 " + std::to_string(code) + " ") == 0, wire); }
std::string body(const std::string& wire) { return wire.substr(wire.find("\r\n\r\n") + 4); }
template <typename C, typename V, typename F>
void visitJob(DriveFixture& f, std::size_t index, F action) { action(f.job<C, V>(index)); }
template <typename F>
void visit(DriveFixture& f, int operation, std::size_t index, F action) {
    switch (operation) {
    case 0:
        visitJob<UpdateNodeCommand, NodeDetails>(f, index, action);
        break;
    case 1:
        visitJob<CopyFileCommand, NodeDetails>(f, index, action);
        break;
    case 2:
        visitJob<TrashNodeCommand, TrashView>(f, index, action);
        break;
    case 3:
        visitJob<RestoreTrashCommand, NodeDetails>(f, index, action);
        break;
    case 4:
        visitJob<PurgeTrashCommand, void>(f, index, action);
        break;
    case 5:
        visitJob<GetTrashCommand, TrashView>(f, index, action);
        break;
    case 6:
        visitJob<SearchNodesCommand, Page<NodeDetails, SearchCursor>>(f, index, action);
        break;
    default:
        visitJob<ListTrashCommand, Page<TrashView, TrashCursor>>(f, index, action);
        break;
    }
}
void success(DriveFixture& f, int operation, std::size_t index = 0) {
    visit(f, operation, index, [&](auto job) {
        using C = decltype(job->command);
        if constexpr (std::is_same_v<C, UpdateNodeCommand> || std::is_same_v<C, CopyFileCommand> || std::is_same_v<C, RestoreTrashCommand>)
            job->result.emplace(driveNodeView());
        else if constexpr (std::is_same_v<C, TrashNodeCommand> || std::is_same_v<C, GetTrashCommand>)
            job->result.emplace(driveTrashView());
        else if constexpr (std::is_same_v<C, PurgeTrashCommand>)
            job->result.emplace(Result<void>{});
        else if constexpr (std::is_same_v<C, SearchNodesCommand>)
            job->result.emplace(Page<NodeDetails, SearchCursor>{ { driveNodeView() }, {} });
        else
            job->result.emplace(Page<TrashView, TrashCursor>{ { driveTrashView() }, {} });
        f.dispatcher.queue.release(job);
    });
}
void submit(DriveFixture& f, Client& c, int operation, std::size_t index = 0, bool wrong = false, bool close = true) {
    c.send(driveRequest(operation, wrong, close));
    f.until([&] { return f.dispatcher.queue.jobs.size() == index + 1; });
}
}  // namespace
BOOST_AUTO_TEST_CASE(allEightOverloadsSuccessHeadersAndBodies) {
    DriveFixture f;
    for (int operation = 0; operation < 8; ++operation) {
        Client c(f.server.listenAddress());
        submit(f, c, operation, static_cast<std::size_t>(operation));
        visit(f, operation, static_cast<std::size_t>(operation), [](auto job) { BOOST_CHECK(job->context.credential.mode == CredentialMode::Bearer); });
        success(f, operation, static_cast<std::size_t>(operation));
        f.until([&] {c.read();return c.eof; });
        status(c.bytes, operation == 1 ? 201 : operation == 4 ? 204
                                                              : 200);
        BOOST_CHECK(c.bytes.find("cache-control: no-store") != std::string::npos);
        BOOST_CHECK(c.bytes.find("x-request-id:") != std::string::npos);
        if (operation == 4) {
            BOOST_CHECK(body(c.bytes).empty());
            BOOST_CHECK(c.bytes.find("content-type:") == std::string::npos);
        } else {
            auto json = Json::parse(body(c.bytes));
            if (operation >= 6) {
                BOOST_CHECK_EQUAL(json["items"].size(), 1);
                BOOST_CHECK(c.bytes.find("\r\netag:") == std::string::npos);
            } else
                BOOST_CHECK(c.bytes.find("etag: " + json["etag"].get<std::string>()) != std::string::npos);
        }
        if (operation == 1 || operation == 3) BOOST_CHECK(c.bytes.find("location: " + nodePath) != std::string::npos);
        if (operation == 2) BOOST_CHECK(c.bytes.find("location: " + trashPath) != std::string::npos);
    }
    BOOST_CHECK_EQUAL((f.job<SearchNodesCommand, Page<NodeDetails, SearchCursor>>(6)->command.text.value()), "%_\\ ");
    BOOST_CHECK((f.job<ListTrashCommand, Page<TrashView, TrashCursor>>(7)->command.root == driveNode));
    BOOST_CHECK((!f.job<RestoreTrashCommand, NodeDetails>(3)->command.parent));
}
BOOST_AUTO_TEST_CASE(allFiveWritesPreserveWrongKindUntilApplicationResult) {
    for (int operation = 0; operation < 5; ++operation)
        for (auto error : { ErrorCode::Unauthenticated, ErrorCode::NotFound, ErrorCode::Forbidden, ErrorCode::PreconditionFailed }) {
            DriveFixture f;
            Client c(f.server.listenAddress());
            submit(f, c, operation, 0, true);
            visit(f, operation, 0, [&](auto job) {
                using C = decltype(job->command);
                if constexpr (std::is_same_v<C, UpdateNodeCommand> || std::is_same_v<C, CopyFileCommand> || std::is_same_v<C, TrashNodeCommand> || std::is_same_v<C, RestoreTrashCommand> || std::is_same_v<C, PurgeTrashCommand>) {
                    BOOST_CHECK(std::holds_alternative<NodeCondition>(job->command.condition) == (operation >= 3));
                    std::visit([](auto value) {BOOST_CHECK_EQUAL(value.id.toString(),driveNode.toString());BOOST_CHECK_EQUAL(value.revision.value(),3); }, job->command.condition);
                }
                job->result.emplace(error);
                f.dispatcher.queue.release(job);
            });
            f.until([&] {c.read();return c.eof; });
            status(c.bytes, error == ErrorCode::Unauthenticated ? 401 : error == ErrorCode::NotFound ? 404
                                                                    : error == ErrorCode::Forbidden  ? 403
                                                                                                     : 412);
            BOOST_CHECK(c.bytes.find("etag:") == std::string::npos);
            BOOST_CHECK(Json::parse(body(c.bytes))["error"]["details"].empty());
        }
}
BOOST_AUTO_TEST_CASE(strictInputAndSecurityRejectWithoutDispatcher) {
    DriveFixture f;
    for (int operation = 0; operation < 8; ++operation) {
        auto input = driveRequest(operation);
        input.erase(input.find(uploadAuth), uploadAuth.size());
        status(f.exchange(input), 401);
        input = driveRequest(operation);
        input.insert(input.find("\r\n\r\n"), "\r\nCookie: __Host-anof_session=x");
        status(f.exchange(input), 400);
        if (operation < 5) {
            input = driveRequest(operation);
            auto start = input.find("If-Match:");
            auto end = input.find("\r\n", start);
            input.erase(start, end - start + 2);
            status(f.exchange(input), 428);
            input = driveRequest(operation);
            start = input.find("If-Match:");
            end = input.find("\r\n", start);
            input.replace(start, end - start, "If-Match: *");
            status(f.exchange(input), 400);
        }
    }
    status(f.exchange(uploadRequest("PATCH", nodePath, "{}", "Content-Type: application/json\r\nIf-Match: " + nodeTag + "\r\n")), 400);
    status(f.exchange(uploadRequest("DELETE", trashPath, "x", "If-Match: " + trashTag + "\r\n")), 413);
    status(f.exchange(uploadRequest("GET", trashPath + "?x=1")), 400);
    status(f.exchange(uploadRequest("GET", "/api/v1/search?space_id=" + uploadSpace.toString() + "&q=x&q=y")), 400);
    status(f.exchange(uploadRequest("HEAD", trashPath)), 405);
    BOOST_CHECK(f.dispatcher.queue.jobs.empty());
}
BOOST_AUTO_TEST_CASE(newErrorsUnknownCommitAndMetadataStorageFull) {
    const std::pair<ErrorCode, int> errors[] = { { ErrorCode::CrossSpaceMove, 409 }, { ErrorCode::DirectoryCycle, 409 }, { ErrorCode::TreeLimitExceeded, 409 }, { ErrorCode::NodeLimitReached, 409 }, { ErrorCode::RestoreTargetUnavailable, 409 }, { ErrorCode::QueryLimitExceeded, 503 }, { ErrorCode::StorageFull, 507 }, { ErrorCode::CommitOutcomeUnknown, 500 }, { ErrorCode::Retryable, 503 } };
    DriveFixture f;
    for (auto [error, code] : errors) {
        auto index = f.dispatcher.queue.jobs.size();
        Client c(f.server.listenAddress());
        submit(f, c, 0, index);
        auto job = f.job<UpdateNodeCommand, NodeDetails>(index);
        job->result.emplace(error);
        f.dispatcher.queue.release(job);
        f.until([&] {c.read();return c.eof; });
        status(c.bytes, code);
        auto json = Json::parse(body(c.bytes));
        BOOST_CHECK(json["error"]["details"].empty());
        if (error == ErrorCode::CommitOutcomeUnknown) BOOST_CHECK_EQUAL(json["error"]["code"], "COMMIT_OUTCOME_UNKNOWN");
    }
}
BOOST_AUTO_TEST_CASE(allOverloadsQueueFullAndStoppingRejectWithoutCallback) {
    DriveFixture f;
    for (auto error : { SubmitError::QueueFull, SubmitError::Stopping, SubmitError::RateLimited })
        for (int operation = 0; operation < 8; ++operation) {
            f.dispatcher.queue.rejection = error;
            status(f.exchange(driveRequest(operation)), error == SubmitError::RateLimited ? 429 : 503);
        }
    BOOST_CHECK(f.dispatcher.queue.jobs.empty());
}
BOOST_AUTO_TEST_CASE(timeoutDisconnectAndForcedStopConsumeLateWriteResults) {
    for (int mode = 0; mode < 3; ++mode)
        for (bool running : { false, true }) {
            HttpApiOptions options;
            options.responseSeconds = 0.04;
            DriveFixture f(options);
            Client c(f.server.listenAddress());
            submit(f, c, 2);
            auto job = f.job<TrashNodeCommand, TrashView>();
            job->running = running;
            if (mode == 1) ::shutdown(c.fd, SHUT_RDWR);
            if (mode == 2) f.server.stop(0, [&] { f.stopped = true; });
            f.until([&] {c.read();return f.finished==1 && c.eof; });
            const auto wire = c.bytes;
            if (!running) BOOST_CHECK(job->cancelled);
            success(f, 2);
            f.until([&] { return f.dispatcher.queue.deliveries == 1; });
            c.read();
            BOOST_CHECK_EQUAL(c.bytes, wire);
            BOOST_CHECK(c.bytes.find("location:") == std::string::npos);
        }
}
BOOST_AUTO_TEST_CASE(graceAllowsAcceptedWriteAndNewRequestsCheckReady) {
    DriveFixture f;
    f.ready = false;
    status(f.exchange(driveRequest(0)), 503);
    BOOST_CHECK(f.dispatcher.queue.jobs.empty());
    f.ready = true;
    Client c(f.server.listenAddress());
    submit(f, c, 3);
    f.ready = false;
    f.server.stop(0.5, [&] { f.stopped = true; });
    success(f, 3);
    f.until([&] {c.read();return c.eof&&f.stopped; });
    status(c.bytes, 200);
}
BOOST_AUTO_TEST_CASE(keepAliveAndBoundedAdmissionAreSharedWithExistingRoutes) {
    HttpApiOptions options;
    options.maxInFlight = 1;
    DriveFixture f(options);
    Client c(f.server.listenAddress());
    submit(f, c, 4, 0, false, false);
    status(f.exchange(uploadRequest("GET", "/health/live")), 503);
    success(f, 4);
    f.until([&] {c.read();return c.bytes.find("\r\n\r\n")!=std::string::npos; });
    status(c.bytes, 204);
    auto boundary = c.bytes.size();
    submit(f, c, 5, 1);
    success(f, 5, 1);
    f.until([&] {c.read();return c.eof; });
    status(c.bytes.substr(boundary), 200);
    BOOST_CHECK_EQUAL(Json::parse(body(c.bytes.substr(boundary)))["id"], driveTrash.toString());
}
BOOST_AUTO_TEST_CASE(copyIntegrityErrorIsDistinctFromOtherDriveFailures) {
    DriveFixture f;
    for (int operation : { 0, 1 }) {
        Client client(f.server.listenAddress());
        const auto index = f.dispatcher.queue.jobs.size();
        submit(f, client, operation, index);
        visit(f, operation, index, [&](auto job) { job->result.emplace(ErrorCode::IntegrityError); f.dispatcher.queue.release(job); });
        f.until([&] { client.read(); return client.eof; });
        status(client.bytes, 500);
        BOOST_CHECK_EQUAL(Json::parse(body(client.bytes))["error"]["code"], operation == 1 ? "INTEGRITY_ERROR" : "INTERNAL_ERROR");
    }
}
