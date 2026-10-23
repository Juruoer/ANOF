#include <anof/client/DriveProtocol.h>
#include <anof/client/internal/Json.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <map>
#include <set>
namespace anof::client {
namespace {
using namespace internal;
bool writes(DriveOperation op) { return op != DriveOperation::Search && op != DriveOperation::ListTrash && op != DriveOperation::ShowTrash; }
std::string header(const HttpResponse& r, const char* key) { auto i=r.headers.find(key); require(i!=r.headers.end()); return i->second; }
Trash trash(const Json& j) {
    exactKeys(j,{"id","instance_id","space_id","root_node_id","original_parent_id","name","kind","deleted_at","expires_at","revision","etag","node_count","logical_bytes"});
    const auto kind=string(j,"kind"); require(kind=="file"||kind=="directory");
    Trash t{id<TrashId>(field(j,"id")),id<InstanceId>(field(j,"instance_id")),id<SpaceId>(field(j,"space_id")),
        id<NodeId>(field(j,"root_node_id")),id<NodeId>(field(j,"original_parent_id")),string(j,"name"),kind=="file",
        time(field(j,"deleted_at")),time(field(j,"expires_at")),Revision::from(decimal(string(j,"revision"),1)).value(),string(j,"etag"),
        NodeCount::from(decimal(string(j,"node_count"),1)).value(),ByteCount::from(decimal(string(j,"logical_bytes"))).value()};
    require(bool(validateDirectoryName(t.name)) && t.deletedAt<t.expiresAt && t.nodeCount.value()<=1000);
    require(!t.file || t.nodeCount.value()==1);
    require(t.etag=="\"trash-"+t.id.toString()+"-"+std::to_string(t.revision.value())+"\"");
    return t;
}
template<class T, class Parse> Page<T> page(const Json& j,unsigned limit,Parse parse) {
    exactKeys(j,{"items","next_cursor"}); const auto& items=field(j,"items");
    require(items.is_array() && items.size()<=limit && items.size()<=200);
    Page<T> p; std::set<std::string> ids;
    for(const auto& item:items) { auto value=parse(item); require(ids.insert(value.id.toString()).second); p.items.push_back(std::move(value)); }
    if(!field(j,"next_cursor").is_null()) {
        auto c=string(j,"next_cursor");require(!items.empty()&&!c.empty()&&c.size()<=2048&&c.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")==std::string::npos);p.nextCursor=c;
    }
    return p;
}
}
ClientResult<ConditionTag> driveCondition(std::string_view s) {
    if(s.substr(0,6)!="\"node-" && s.substr(0,7)!="\"trash-") return failure(ErrorKind::InvalidArgument);
    return ConditionTag::parse(s);
}
ClientResult<Reply<DrivePayload>> decodeDriveResponse(DriveOperation op,const HttpResponse& r,unsigned limit) {
    try {
        require(r.body.view().size()<=kResponseLimit);
        auto rid=RequestId::parse(header(r,"x-request-id"));require(rid&&rid.value().toString()==header(r,"x-request-id"));
        require(!r.headers.count("content-encoding")||header(r,"content-encoding")=="identity");
        ResponseMeta meta{r.status,rid.value(),{}};
        if(r.headers.count("etag")) {meta.etag=header(r,"etag");require(meta.etag->size()<=128);}
        const int expected=op==DriveOperation::Copy?201:op==DriveOperation::Purge?204:200;
        if(r.status==204) {
            require(expected==204&&r.body.view().empty()&&!meta.etag&&!r.headers.count("transfer-encoding")&&!r.headers.count("content-type"));
            if(r.headers.count("content-length"))require(decimal(header(r,"content-length"))==0);
            return Reply<DrivePayload>{Done{},meta};
        }
        require(jsonContentType(header(r,"content-type")));
        Document doc;doc.parse(r.body.view());const auto& j=doc.root;
        if(r.status!=expected) {
            const auto& e=field(j,"error");auto code=string(e,"code");
            static const std::map<std::string,int> codes{{"INVALID_ARGUMENT",400},{"INVALID_NAME",400},{"UNAUTHENTICATED",401},{"FORBIDDEN",403},{"QUOTA_EXCEEDED",403},{"NOT_FOUND",404},{"NAME_CONFLICT",409},{"PARENT_NOT_DIRECTORY",409},{"DEPTH_LIMIT_EXCEEDED",409},{"NOT_A_FILE",409},{"CROSS_SPACE_MOVE",409},{"DIRECTORY_CYCLE",409},{"TREE_LIMIT_EXCEEDED",409},{"NODE_LIMIT_REACHED",409},{"RESTORE_TARGET_UNAVAILABLE",409},{"PRECONDITION_FAILED",412},{"PRECONDITION_REQUIRED",428},{"REQUEST_TOO_LARGE",413},{"UNSUPPORTED_MEDIA_TYPE",415},{"RATE_LIMITED",429},{"INTERNAL_ERROR",500},{"COMMIT_OUTCOME_UNKNOWN",500},{"TEMPORARILY_UNAVAILABLE",503},{"QUERY_LIMIT_EXCEEDED",503},{"STORAGE_FULL",507}};
            auto i=codes.find(code);require(i!=codes.end()&&i->second==r.status&&id<RequestId>(field(e,"request_id"))==meta.requestId);
            require(field(e,"message").is_string()&&field(e,"details").is_object()&&field(e,"details").empty());
            require(code!="COMMIT_OUTCOME_UNKNOWN"||writes(op));
            auto error=failure(code=="COMMIT_OUTCOME_UNKNOWN"?ErrorKind::OutcomeUnknown:ErrorKind::Application,code);
            error.remote=code=="COMMIT_OUTCOME_UNKNOWN"?RemoteOutcome::Unknown:RemoteOutcome::Rejected;
            error.httpStatus=r.status;error.requestId=meta.requestId;return error;
        }
        DrivePayload value=Done{};
        if(op==DriveOperation::Search) {
            require(!meta.etag);auto p=page<Node>(j,limit,parseNode);
            for(std::size_t i=0;i<p.items.size();++i) {require(p.items[i].parentId.has_value());if(i)require(std::tie(p.items[i-1].name,p.items[i-1].id)<std::tie(p.items[i].name,p.items[i].id));}
            value=std::move(p);
        } else if(op==DriveOperation::ListTrash) {
            require(!meta.etag);auto p=page<Trash>(j,limit,trash);
            for(std::size_t i=1;i<p.items.size();++i)require(std::tie(p.items[i-1].deletedAt,p.items[i-1].id)<std::tie(p.items[i].deletedAt,p.items[i].id));
            value=std::move(p);
        } else if(op==DriveOperation::Remove||op==DriveOperation::ShowTrash) {
            auto t=trash(j);require(meta.etag&&*meta.etag==t.etag);
            if(op==DriveOperation::Remove)require(header(r,"location")=="/api/v1/trash/"+t.id.toString());
            value=std::move(t);
        } else {
            require(op!=DriveOperation::Purge);auto n=parseNode(j);require(meta.etag&&*meta.etag==n.etag&&n.parentId);
            if(op==DriveOperation::Copy||op==DriveOperation::Restore)require(header(r,"location")=="/api/v1/nodes/"+n.id.toString());
            if(op==DriveOperation::Copy)require(n.file.has_value());
            value=std::move(n);
        }
        return Reply<DrivePayload>{std::move(value),meta};
    } catch(...) {return failure(writes(op)?ErrorKind::OutcomeUnknown:ErrorKind::Protocol);}
}
} // namespace anof::client
