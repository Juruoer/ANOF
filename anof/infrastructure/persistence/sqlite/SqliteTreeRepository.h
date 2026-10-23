#pragma once
#include <anof/application/ports/TreeRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransactionContext.h>
namespace anof::infrastructure::sqlite {
class SqliteTreeRepository final : public TreeRepository {
public:
    explicit SqliteTreeRepository(SqliteTransactionContext& c) : context_(c) {}
    Result<TreeSnapshot> activeSubtree(NodeId, QueryBudget) override;
    Result<TreeSnapshot> trashSubtree(TrashId, QueryBudget) override;
    Result<Page<NodeDetails, ChildPosition>> search(const Principal&, const SearchQuery&) override;
    Result<NodeDetails> move(const TreeMutationPlan&) override;
    Result<void> detach(const TreeSnapshot&, NodeCondition, const TrashEntry&) override;
    Result<NodeDetails> restore(const TreeMutationPlan&, TrashCondition) override;
    Result<void> eraseMembers(TrashCondition, const TreeSnapshot&) override;

private:
    SqliteTransactionContext& context_;
};
}  // namespace anof::infrastructure::sqlite
