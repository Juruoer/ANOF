#pragma once
#include <anof/transport/http/DriveRequest.h>
#include <anof/transport/http/DriveResponse.h>
#include <anof/transport/http/Pagination.h>

#include "UploadTestSupport.h"
namespace anof::http::testing {
inline const auto driveUser = UserId::parse("40000000-0000-4000-8000-000000000001").value();
inline const auto driveNode = NodeId::parse("30000000-0000-4000-8000-000000000001").value();
// 相同 UUID/revision 的异类条件必须仍保持不同 alternative。
inline const auto driveTrash = TrashId::parse(driveNode.toString()).value();
inline const std::string nodePath = "/api/v1/nodes/" + driveNode.toString();
inline const std::string trashPath = "/api/v1/trash/" + driveTrash.toString();
inline const std::string nodeTag = "\"node-" + driveNode.toString() + "-3\"";
inline const std::string trashTag = "\"trash-" + driveTrash.toString() + "-3\"";
inline NodeDetails driveNodeView() {
    return { { driveNode, uploadSpace, uploadParent, "家庭", driveUser, Revision::from(3).value(), UtcTime{}, UtcTime{} }, DirectoryContent{} };
}
inline TrashView driveTrashView() {
    return { instanceId, { driveTrash, uploadSpace, driveNode, uploadParent, "家庭", NodeKind::Directory, driveUser, UtcTime{}, trashExpiry(UtcTime{}).value(), Revision::from(3).value(), NodeCount::from(2).value(), ByteCount::from(123).value(), driveUser } };
}
class DriveFake final : public DriveDispatcher {
public:
    UploadFake queue;
    explicit DriveFake(EventLoop& loop) : queue(loop) {}
    TaskSubmission submit(RequestContext context, UpdateNodeCommand cmd, Completion<NodeDetails> cb) override { return queue.accept<UpdateNodeCommand, NodeDetails>(context, std::move(cmd), std::move(cb)); }
    TaskSubmission submit(RequestContext context, CopyFileCommand cmd, Completion<NodeDetails> cb) override { return queue.accept<CopyFileCommand, NodeDetails>(context, std::move(cmd), std::move(cb)); }
    TaskSubmission submit(RequestContext context, TrashNodeCommand cmd, Completion<TrashView> cb) override { return queue.accept<TrashNodeCommand, TrashView>(context, std::move(cmd), std::move(cb)); }
    TaskSubmission submit(RequestContext context, RestoreTrashCommand cmd, Completion<NodeDetails> cb) override { return queue.accept<RestoreTrashCommand, NodeDetails>(context, std::move(cmd), std::move(cb)); }
    TaskSubmission submit(RequestContext context, PurgeTrashCommand cmd, Completion<void> cb) override { return queue.accept<PurgeTrashCommand, void>(context, std::move(cmd), std::move(cb)); }
    TaskSubmission submit(RequestContext context, GetTrashCommand cmd, Completion<TrashView> cb) override { return queue.accept<GetTrashCommand, TrashView>(context, std::move(cmd), std::move(cb)); }
    TaskSubmission submit(RequestContext context, SearchNodesCommand cmd, Completion<Page<NodeDetails, SearchCursor>> cb) override { return queue.accept<SearchNodesCommand, Page<NodeDetails, SearchCursor>>(context, std::move(cmd), std::move(cb)); }
    TaskSubmission submit(RequestContext context, ListTrashCommand cmd, Completion<Page<TrashView, TrashCursor>> cb) override { return queue.accept<ListTrashCommand, Page<TrashView, TrashCursor>>(context, std::move(cmd), std::move(cb)); }
};
class DriveFixture {
public:
    EventLoop loop;
    Codec codec;
    Identity identity;
    UnexpectedIdentityDispatcher identities;
    UnexpectedTransferDispatcher transfers;
    DriveFake dispatcher{ loop };
    bool ready = true, stopped = false;
    std::size_t finished = 0;
    HttpApi api;
    HttpServer server{ &loop, InetAddress(0, true), "drive-test" };
    explicit DriveFixture(HttpApiOptions options = {}) : api(codec, identity, [this] { return ready; }, [](auto, const auto&, const auto&) -> HttpStreamCallbacks { BOOST_ERROR("Unexpected mkdir"); return {}; }, identities, transfers, dispatcher, developmentConfiguration(), { {}, [this](auto) { ++finished; } }, options) {
        api.configure(server);
        server.start();
    }
    void until(const std::function<bool()>& done, const char* file = __builtin_FILE(), int line = __builtin_LINE()) {
        BOOST_REQUIRE(anof::test::waitFor(loop, done, 3, file, line));
    }
    ~DriveFixture() {
        server.stop(0, [this] { stopped = true; });
        for (auto& job : dispatcher.queue.jobs) dispatcher.queue.release(job);
        until([&] { return stopped && dispatcher.queue.deliveries == dispatcher.queue.jobs.size(); });
    }
    template <typename C, typename V>
    auto job(std::size_t index = 0) {
        auto value = std::dynamic_pointer_cast<UploadTypedJob<C, V>>(dispatcher.queue.jobs.at(index));
        BOOST_REQUIRE(value);
        return value;
    }
    std::string exchange(std::string input) {
        Client client(server.listenAddress());
        client.send(input);
        until([&] { client.read(); return client.eof; });
        return client.bytes;
    }
};
inline std::string driveRequest(int operation, bool wrongKind = false, bool close = true) {
    const std::string condition = "If-Match: " + ((operation < 3) != wrongKind ? nodeTag : trashTag) + "\r\nContent-Type: application/json\r\n";
    switch (operation) {
    case 0:
        return uploadRequest("PATCH", nodePath, "{\"name\":\"家庭\"}", condition, close);
    case 1:
        return uploadRequest("POST", nodePath + "/copy", "{\"space_id\":\"" + uploadSpace.toString() + "\",\"parent_id\":\"" + uploadParent.toString() + "\",\"name\":\"copy\"}", condition, close);
    case 2:
        return uploadRequest("DELETE", nodePath, {}, condition, close);
    case 3:
        return uploadRequest("POST", trashPath + "/restore", "{}", condition, close);
    case 4:
        return uploadRequest("DELETE", trashPath, {}, condition, close);
    case 5:
        return uploadRequest("GET", trashPath, {}, {}, close);
    case 6:
        return uploadRequest("GET", "/api/v1/search?space_id=" + uploadSpace.toString() + "&q=%25_%5C%20", {}, {}, close);
    default:
        return uploadRequest("GET", "/api/v1/trash?space_id=" + uploadSpace.toString() + "&root_node_id=" + driveNode.toString(), {}, {}, close);
    }
}
}  // namespace anof::http::testing
