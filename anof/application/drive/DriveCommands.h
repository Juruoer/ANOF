#pragma once
#include <variant>
#include <anof/application/RequestContext.h>
#include <anof/application/transfer/TransferCommands.h>
#include <anof/domain/drive/TreePolicy.h>
#include <anof/domain/drive/NodeAccounting.h>
namespace anof {
// 物理账本ETag不绑定派生GC/节点计数；业务接线时在同一metadata快照读取。
struct DriveStorageView { StorageView storage; GcStatistics gc; NodeAccounting nodes; };
// 保留标签资源种类；仅应用在鉴权后解包为仓储的同类条件。
using DriveCondition = std::variant<NodeCondition, TrashCondition>;
struct UpdateNodeCommand { NodeId id; DriveCondition condition; std::optional<NodeId> parent; std::optional<std::string> name; };
struct CopyFileCommand { NodeId id; DriveCondition condition; SpaceId space; NodeId parent; std::string name; };
struct TrashNodeCommand { NodeId id; DriveCondition condition; };
struct RestoreTrashCommand { TrashId id; DriveCondition condition; std::optional<NodeId> parent; std::optional<std::string> name; };
struct PurgeTrashCommand { TrashId id; DriveCondition condition; };
struct GetTrashCommand { TrashId id; };
struct TrashView { InstanceId instanceId; TrashEntry entry; };
struct SearchCursor { UserId user; SpaceId space; SearchText text; ChildPosition position; };
struct TrashCursor { UserId user; SpaceId space; std::optional<NodeId> root; TrashPosition position; };
struct SearchNodesCommand { SpaceId space; SearchText text; PageSize limit; std::optional<SearchCursor> after; };
struct ListTrashCommand { SpaceId space; std::optional<NodeId> root; PageSize limit; std::optional<TrashCursor> after; };
// 条件绑定必须在用例鉴权后校验；此函数不替代授权，也不直接供HTTP提前判定资源版本。
Result<void> validateDriveCommand(const UpdateNodeCommand&);
Result<void> validateDriveCommand(const CopyFileCommand&);
Result<void> validateDriveCommand(const RestoreTrashCommand&);
Result<void> validateDriveCommand(UserId, const SearchNodesCommand&);
Result<void> validateDriveCommand(UserId, const ListTrashCommand&);
}  // namespace anof
