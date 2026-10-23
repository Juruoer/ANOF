#include <apps/anof/ClientOutput.h>
#include <anof/client/HttpTransport.h>
#include <cerrno>
#include <unistd.h>
namespace anof::app {
using namespace client;
using Json = nlohmann::json;
namespace {
const char* remoteName(RemoteOutcome value) {
    switch (value) { case RemoteOutcome::NotSent:return "not_sent"; case RemoteOutcome::Rejected:return "rejected"; case RemoteOutcome::ConfirmedSuccess:return "confirmed_success"; case RemoteOutcome::Unknown:return "unknown"; }
    return "unknown";
}
const char* localName(LocalState value) {
    switch (value) { case LocalState::Unchanged:return "unchanged"; case LocalState::CredentialSaved:return "credential_saved"; case LocalState::CredentialCleared:return "credential_cleared"; case LocalState::TransferSaved:return "transfer_saved"; case LocalState::DownloadPartial:return "download_partial"; case LocalState::FilePublished:return "file_published"; case LocalState::Unknown:return "unknown"; }
    return "unknown";
}
bool writeAll(int fd, std::string_view bytes) noexcept {
    while (!bytes.empty()) { const auto n = ::write(fd,bytes.data(),bytes.size()); if (n <= 0) return false; bytes.remove_prefix(static_cast<std::size_t>(n)); }
    return true;
}
Json amount(const Amount& value) { return {{"bytes",std::to_string(value.bytes.value())},{"nodes",std::to_string(value.nodes.value())}}; }
Json quota(const Quota& value) { return {{"limit",amount(value.limit)},{"used",amount(value.used)},{"reserved",amount(value.reserved)},{"revision",std::to_string(value.revision.value())}}; }
void readable(std::string& out, const Json& j, const std::string& prefix = {}) {
    if (j.is_array()) {
        if (j.empty()) out += prefix + "(empty)\n";
        for (std::size_t i = 0; i < j.size(); ++i) readable(out,j[i],prefix + "[" + std::to_string(i + 1) + "].");
    } else if (j.is_object()) {
        for (auto it=j.begin(); it!=j.end(); ++it) {
            if (it.key()=="next_cursor" && it.value().is_null()) out += prefix + "next_cursor: (end)\n";
            else readable(out,it.value(),prefix + it.key() + (it.value().is_structured()?".":""));
        }
    } else out += prefix + ": " + j.dump(-1,' ',false) + "\n";
}
}
Json clientData(const User& u) { return {{"id",u.id.toString()},{"login",u.login.value()},{"display_name",u.displayName.value()},{"role",u.role==User::Role::Admin?"administrator":"member"},{"enabled",u.enabled},{"revision",std::to_string(u.revision.value())},{"created_at",u.createdAt},{"updated_at",u.updatedAt}}; }
Json clientData(const AdminUser& v) { return {{"user",clientData(v.user)},{"private_space_id",v.privateSpaceId.toString()}}; }
Json clientData(const ManagedQuota& v) { return {{"space_id",v.target.spaceId.toString()},{"user_id",v.target.userId?Json(v.target.userId->toString()):Json(nullptr)},{"quota",quota(v.quota)}}; }
Json clientData(const SavedLogin& v) { return {{"user",clientData(v.user)},{"expires_at",v.expiresAt},{"credential_saved",v.credentialSaved}}; }
Json clientData(const Invite& v) { return {{"id",v.id.toString()},{"created_by",v.createdBy.toString()},{"created_at",v.createdAt},{"expires_at",v.expiresAt},{"max_uses",v.maxUses.value()},{"used_count",v.usedCount.value()},{"revoked_at",v.revokedAt?Json(*v.revokedAt):Json(nullptr)}}; }
Json clientData(const Space& v) { return {{"id",v.id.toString()},{"kind",v.kind==Space::Kind::Private?"private":"public"},{"owner_id",v.ownerId?Json(v.ownerId->toString()):Json(nullptr)},{"root_node_id",v.rootNodeId.toString()},{"quota",quota(v.quota)},{"my_contribution_quota",v.myContributionQuota?quota(*v.myContributionQuota):Json(nullptr)}}; }
Json clientData(const Node& v) {
    Json j{{"id",v.id.toString()},{"space_id",v.spaceId.toString()},{"parent_id",v.parentId?Json(v.parentId->toString()):Json(nullptr)},{"name",v.name},{"created_by",v.createdBy.toString()},{"created_at",v.createdAt},{"updated_at",v.updatedAt},{"etag",v.etag},{"kind",v.file?"file":"directory"}};
    if (v.file) { j["size"]=std::to_string(v.file->size.value()); j["sha256"]=v.file->sha256; } return j;
}
Json clientData(const ProfileSummary& v) {
    Json session = nullptr;
    if (v.session) session={{"expires_at",v.session->expiresAt},{"user_id",v.session->userId.toString()},{"login",v.session->login.value()}};
    return {{"name",v.name},{"server",v.server},{"development_http",v.developmentHttp},{"session",std::move(session)}};
}
Json clientData(const Trash& t) {return {{"id",t.id.toString()},{"instance_id",t.instanceId.toString()},{"space_id",t.spaceId.toString()},{"root_node_id",t.rootNodeId.toString()},{"original_parent_id",t.originalParentId.toString()},{"name",t.name},{"kind",t.file?"file":"directory"},{"deleted_at",t.deletedAt},{"expires_at",t.expiresAt},{"revision",std::to_string(t.revision.value())},{"etag",t.etag},{"node_count",std::to_string(t.nodeCount.value())},{"logical_bytes",std::to_string(t.logicalBytes.value())}};}
Json clientData(Done) { return nullptr; }
ClientError ClientOutput::outputFailure() const {
    auto e = failure(ErrorKind::LocalState); e.remote=remote; e.local=local; e.transferId=transferId;
    if (confirmed) { e.httpStatus=confirmed->httpStatus; e.requestId=confirmed->requestId; } return e;
}
int ClientOutput::error(const ClientError& e) const noexcept {
    try {
        std::string message = errorMessage(e.kind);
        if (e.remote == RemoteOutcome::Unknown && e.kind != ErrorKind::OutcomeUnknown) message += "; remote outcome unknown; no automatic retry";
        if (passwordChange && e.remote == RemoteOutcome::Unknown) message += "; verify the password/session state or explicitly log in again";
        if (e.profileName && ProfileStore::validName(*e.profileName)) message += " (profile: " + *e.profileName + ")";
        if(e.transferId) message += " (transfer: " + *e.transferId + ")";
        if(e.code=="UPLOAD_RESULT_DELETED")message="Upload historically completed, but its result has been permanently deleted; no retransmission performed";
        if(e.remote==RemoteOutcome::Unknown&&!recovery.empty())message+="; "+recovery;
        std::string text;
        if (json_) text=Json{{"ok",false},{"error",{{"code",e.code},{"message",message},{"http_status",e.httpStatus?Json(*e.httpStatus):Json(nullptr)},{"request_id",e.requestId?Json(e.requestId->toString()):Json(nullptr)},{"remote_outcome",remoteName(e.remote)},{"local_state",localName(e.local)},{"transfer_id",e.transferId?Json(*e.transferId):Json(nullptr)}}}}.dump()+"\n";
        else text=e.code+": "+message+"; remote_outcome="+remoteName(e.remote)+"; local_state="+localName(e.local)+"\n";
        if(operation) {
            if(json_) {auto j=Json::parse(text);j["error"]["operation"]=*operation;j["error"]["source_id"]=sourceId?Json(*sourceId):Json(nullptr);text=j.dump()+"\n";}
            else text="operation="+*operation+"; source_id="+sourceId.value_or("none")+"; "+text;
        }
        (void)writeAll(STDERR_FILENO,text);
    } catch (...) { (void)writeAll(STDERR_FILENO,"{\"ok\":false,\"error\":{\"code\":\"LOCAL_STATE_ERROR\",\"message\":\"Client output failed\",\"http_status\":null,\"request_id\":null,\"remote_outcome\":\"unknown\",\"local_state\":\"unknown\"}}\n"); }
    return e.exitCode();
}
int ClientOutput::help(std::string_view text) const { return writeAll(STDOUT_FILENO,text) ? 0 : error(outputFailure()); }
int ClientOutput::success(const Json& data, std::optional<ResponseMeta> meta, std::string_view invite) const {
    // 邀请秘密直接进入清零缓冲，不放进 JSON DOM 或临时 string。
    SecureBuffer bytes(2 * kResponseLimit + 65536);
    auto append=[&](std::string_view s) { if (!bytes.append(s)) throw std::length_error("Client output limit"); };
    if (json_) {
        append("{\"ok\":true,\"data\":");
        if (!invite.empty()) { append("{\"invite\":"); append(data.dump()); append(",\"token\":\""); append(invite); append("\"}"); }
        else append(data.dump());
        append(",\"meta\":");
        Json metadata{{"profile",profile_?Json(*profile_):Json(nullptr)},{"http_status",meta?Json(meta->httpStatus):Json(nullptr)},{"request_id",meta?Json(meta->requestId.toString()):Json(nullptr)},{"etag",meta&&meta->etag?Json(*meta->etag):Json(nullptr)}};
        if(operation){metadata["operation"]=*operation;metadata["source_id"]=sourceId?Json(*sourceId):Json(nullptr);metadata["remote_outcome"]=remoteName(remote);}
        if(!notice.empty())metadata["notice"]=notice;
        append(metadata.dump()); append("}\n");
    } else {
        if(operation){append("operation="+*operation+"; source_id="+sourceId.value_or("none")+"; remote_outcome="+remoteName(remote)+"\n");}
        if(!notice.empty()){append(notice);append("\n");}
        std::string ordinary; if (data.is_null()) ordinary="OK\n"; else readable(ordinary,data); append(ordinary);
        if (meta && meta->etag) { append("ETag: "); append(*meta->etag); append("\n"); }
        if (!invite.empty()) { append("Invitation token (shown once): "); append(invite); append("\n"); }
    }
    if (!writeAll(STDOUT_FILENO,bytes.view())) return error(outputFailure());
    return 0;
}
}  // namespace anof::app
