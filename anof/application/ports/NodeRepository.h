#pragma once

#include <anof/core/Result.h>
#include <anof/domain/drive/NodeDetails.h>

namespace anof {
class NodeReader {
public:
    virtual ~NodeReader() = default;
    // 路径必须同空间、活动、无隐藏回收站祖先；长度最多129；目标是文件为 ParentNotDirectory。
    virtual Result<DirectoryPath> findActiveDirectoryPath(SpaceId, NodeId parent) = 0;
    virtual Result<std::optional<NodeDetails>> find(NodeId) = 0;
    // 调用者在同一快照先授权和检查父路径；按 UTF-8 名称字节、ID 有界 seek。
    virtual Result<Page<NodeDetails, ChildPosition>> listChildren(SpaceId, NodeId parent, PageSize,
                                                                 std::optional<ChildPosition> after) = 0;
};
class NodeRepository : public NodeReader {
public:
    virtual Result<void> insertRoot(const RootDirectory&) = 0;
    virtual Result<DirectorySnapshot> insertDirectory(const NewDirectory&) = 0;
    // 同事务按nondecreasingEventTime(now, createdAt, updatedAt)更新；revision仍精确+1，时间可相等。
    virtual Result<Revision> advanceRevision(NodeId, Revision expected, UtcTime) = 0;
};
}  // namespace anof
