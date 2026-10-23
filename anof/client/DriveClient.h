#pragma once
#include <anof/client/DriveProtocol.h>
#include <anof/client/ProfileStore.h>
namespace anof::client {
// 同步借用依赖；调用者持有profile锁。一次命令一次请求，不预读条件或自动重试。
class DriveClient {
public:
    DriveClient(HttpTransport& transport, LockedProfile& profile, const std::atomic_bool* cancel = nullptr)
        : transport_(transport), profile_(profile), cancel_(cancel) {}
    ClientResult<Reply<Node>> update(NodeId, std::optional<NodeId> parent, std::optional<std::string> name, const ConditionTag&);
    ClientResult<Reply<Node>> copy(NodeId, SpaceId, NodeId parent, std::string name, const ConditionTag&);
    ClientResult<Reply<Trash>> remove(NodeId, const ConditionTag&);
    ClientResult<Reply<Page<Node>>> search(SpaceId, std::string query, PageRequest page = {});
    ClientResult<Reply<Page<Trash>>> listTrash(SpaceId, std::optional<NodeId> root = {}, PageRequest page = {});
    ClientResult<Reply<Trash>> showTrash(TrashId);
    ClientResult<Reply<Node>> restore(TrashId, std::optional<NodeId> parent, std::optional<std::string> name, const ConditionTag&);
    ClientResult<Reply<Done>> purge(TrashId, const ConditionTag&);
private:
    template<class T> ClientResult<Reply<T>> call(DriveOperation, Method, std::string, SecureBuffer = {}, std::optional<ConditionTag> = {}, unsigned limit = 200);
    HttpTransport& transport_;
    LockedProfile& profile_;
    const std::atomic_bool* cancel_;
};
} // namespace anof::client
