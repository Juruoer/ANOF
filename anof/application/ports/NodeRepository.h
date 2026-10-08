#pragma once

#include <anof/core/Result.h>
#include <anof/domain/drive/Node.h>

namespace anof {

class NodeRepository {
public:
    virtual ~NodeRepository() = default;
    // 缺失／跨空间／任一回收站祖先返回 NotFound；活动文件返回 ParentNotDirectory。
    // 有界读取：根深度 0，最多返回 129 个目录；损坏或更深路径返回 IntegrityError。
    virtual Result<DirectoryPath> findActiveDirectoryPath(SpaceId spaceId, NodeId parentId) = 0;
    // 验证名称、活动父路径和深度；名称与现存文件共用字节唯一性约束。
    // 返回 revision=1、updatedAt=createdAt 的新目录；ID 冲突为 IntegrityError。
    virtual Result<DirectorySnapshot> insertDirectory(const NewDirectory& directory) = 0;
    // 缺失为 NotFound，版本不符为 Conflict，溢出为 IntegrityError。
    virtual Result<Revision> advanceRevision(NodeId id, Revision expected, UtcTime now) = 0;
};

}  // namespace anof
