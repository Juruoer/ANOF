#pragma once
#include <anof/client/ProfileStore.h>
#include <anof/client/TransferProtocol.h>
#include <anof/client/TransferTransport.h>
namespace anof::client {
struct TransferResult {
    nlohmann::json data;  // 本地字段与受检TransferReply组合，不含原始远端message或凭据。
    std::optional<ResponseMeta> meta;
    RemoteOutcome remote = RemoteOutcome::ConfirmedSuccess;
    LocalState local = LocalState::TransferSaved;
};
class TransferClient {
public:
    TransferClient(HttpTransport& http, TransferTransport& binary, LockedProfile& profile, std::string directory,
                   const std::atomic_bool* cancel = nullptr)
        : http_(http), binary_(binary), profile_(profile), directory_(std::move(directory)), cancel_(cancel) {}
    ClientResult<TransferResult> upload(std::string file, SpaceId, NodeId, std::string name);
    ClientResult<TransferResult> download(NodeId, std::string output);
    ClientResult<TransferResult> resume(std::string id, bool download);
    ClientResult<TransferReply> show(UploadId);
    ClientResult<TransferReply> list(PageRequest = {}, std::optional<OperationId> = {});
    ClientResult<TransferReply> complete(UploadId, std::optional<std::pair<NodeId, std::string>> = {});
    ClientResult<TransferReply> cancel(UploadId);
    ClientResult<TransferReply> storage();
    ClientResult<TransferReply> setStorage(ByteCount, std::string tag);

private:
    ClientResult<TransferReply> request(TransferOperation, Method, std::string, const nlohmann::json& = nullptr,
                                        unsigned limit = 200, std::optional<AccountTag> = {});
    TransferResult workflow(std::optional<std::string> id, bool download, std::string file, nlohmann::json target);
    HttpTransport& http_;
    TransferTransport& binary_;
    LockedProfile& profile_;
    std::string directory_;
    const std::atomic_bool* cancel_;
};
}  // namespace anof::client
