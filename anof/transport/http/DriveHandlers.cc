#include <anof/transport/http/DriveHandlers.h>
#include <anof/transport/http/DriveRequest.h>
#include <anof/transport/http/DriveResponse.h>
#include <anof/transport/http/Pagination.h>
#include <anof/transport/http/IdentityStream.h>
#include <anof/transport/http/RequestCredentials.h>
namespace anof::http {
namespace {
using namespace muduox::net;
HttpRequestDecision reject(ProtocolError error, RequestId id) {
    return HttpRequestDecision::respond(errorResponse(error, id).buffered(true));
}
template<typename Command, typename Value, typename Formatter>
HttpRequestDecision read(Parsed<Command> parsed, RequestContext context, DriveDispatcher& dispatcher, Formatter formatter, const std::function<bool()>& ready) {
    if (auto* error = std::get_if<ProtocolError>(&parsed)) return reject(*error, context.requestId);
    return HttpRequestDecision::streaming(makeIdentityStream<Command, Value>(dispatcher, context, RequestBody::None,
        [command = std::get<Command>(std::move(parsed))](auto) -> Parsed<Command> { return command; }, formatter, ready));
}
}
std::optional<muduox::net::HttpRequestDecision> driveDecision(const muduox::net::HttpRequest& request, std::string_view pattern,
    std::optional<std::string_view> routeId, RequestContext context, DriveDispatcher& dispatcher, const std::function<bool()>& ready) {
    using namespace muduox::net;
    const bool node = pattern == "/api/v1/nodes/:node_id" && request.method() != HttpRequest::kGet;
    const bool copy = pattern == "/api/v1/nodes/:node_id/copy";
    const bool trash = pattern == "/api/v1/trash/:node_id";
    const bool restore = pattern == "/api/v1/trash/:node_id/restore";
    const bool search = pattern == "/api/v1/search";
    const bool list = pattern == "/api/v1/trash";
    if (!node && !copy && !trash && !restore && !search && !list) return std::nullopt;
    const bool json = request.method() == HttpRequest::kPatch || request.method() == HttpRequest::kPost;
    const auto id = context.requestId;
    if (auto error = json ? validateJsonContentHeaders(request.headers()) : validateBodylessHeaders(request.headers())) return reject(*error, id);
    if (search) return read<SearchNodesCommand, Page<NodeDetails, SearchCursor>>(parseSearchNodes(request.query()), context, dispatcher, searchResponse, ready);
    if (list) return read<ListTrashCommand, Page<TrashView, TrashCursor>>(parseListTrash(request.query()), context, dispatcher, trashPageResponse, ready);
    if (auto error = validateNoQuery(request.query())) return reject(*error, id);
    if (!routeId) return reject(ProtocolError::InternalError, id);
    if (trash && request.method() == HttpRequest::kGet) {
        auto target = TrashId::parse(*routeId);
        if (!target) return reject(ProtocolError::InvalidArgument, id);
        return read<GetTrashCommand, TrashView>(GetTrashCommand{target.value()}, context, dispatcher, trashResponse, ready);
    }
    auto parsed = parseDriveCondition(request.headers());
    if (auto* error = std::get_if<ProtocolError>(&parsed)) return reject(*error, id);
    auto condition = std::get<DriveCondition>(std::move(parsed));
    if (node || copy) {
        auto target = NodeId::parse(*routeId);
        if (!target) return reject(ProtocolError::InvalidArgument, id);
        if (copy) return HttpRequestDecision::streaming(makeIdentityStream<CopyFileCommand, NodeDetails>(dispatcher, context, RequestBody::Json,
            [target = target.value(), condition](auto body) { return parseCopyFile(body, target, condition); }, copiedNodeResponse, ready,
            [](ErrorCode code) { return code == ErrorCode::IntegrityError ? ProtocolError::IntegrityError : protocolError(code); }));
        if (request.method() == HttpRequest::kDelete) return read<TrashNodeCommand, TrashView>(TrashNodeCommand{target.value(), condition}, context, dispatcher, trashedNodeResponse, ready);
        return HttpRequestDecision::streaming(makeIdentityStream<UpdateNodeCommand, NodeDetails>(dispatcher, context, RequestBody::Json,
            [target = target.value(), condition](auto body) { return parseUpdateNode(body, target, condition); }, nodeResponse, ready));
    }
    auto target = TrashId::parse(*routeId);
    if (!target) return reject(ProtocolError::InvalidArgument, id);
    if (restore) return HttpRequestDecision::streaming(makeIdentityStream<RestoreTrashCommand, NodeDetails>(dispatcher, context, RequestBody::Json,
        [target = target.value(), condition](auto body) { return parseRestoreTrash(body, target, condition); }, restoredNodeResponse, ready));
    return read<PurgeTrashCommand, void>(PurgeTrashCommand{target.value(), condition}, context, dispatcher, noContentResponse, ready);
}
}  // namespace anof::http
