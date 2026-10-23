#pragma once
#include <anof/domain/drive/TreePolicy.h>
namespace anof {
struct SearchQuery { SpaceId spaceId; SearchText text; PageSize limit; std::optional<ChildPosition> after; QueryBudget budget; };
struct TreeMutationPlan {
    TreeSnapshot expected;
    NodeCondition rootCondition;
    NodeId destination;
    std::string name;
    UtcTime occurredAt;
};
class TreeReader {
public:
    virtual ~TreeReader() = default;
    // 返回完整活动树或最多1001个节点的超限证据；不得静默截断成可写树。
    virtual Result<TreeSnapshot> activeSubtree(NodeId, QueryBudget) = 0;
    virtual Result<TreeSnapshot> trashSubtree(TrashId, QueryBudget) = 0;
    virtual Result<Page<NodeDetails, ChildPosition>> search(const Principal&, const SearchQuery&) = 0;
};
class TreeRepository : public TreeReader {
public:
    // 条件计划须在当前事务读取；复核成员ID/revision、名称、空间、祖先和完整行数。
    // 这些操作负责各受影响Node/直接父revision，外层不得重复推进。
    virtual Result<NodeDetails> move(const TreeMutationPlan&) = 0;
    virtual Result<void> detach(const TreeSnapshot&, NodeCondition, const TrashEntry&) = 0;
    virtual Result<NodeDetails> restore(const TreeMutationPlan&, TrashCondition) = 0;
    // 只删除匹配条目的完整成员；外层同事务处理额度、Upload证据、Blob引用及条目行。
    virtual Result<void> eraseMembers(TrashCondition, const TreeSnapshot&) = 0;
};
}  // namespace anof
