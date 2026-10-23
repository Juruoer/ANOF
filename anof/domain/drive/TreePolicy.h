#pragma once
#include <anof/domain/drive/Trash.h>
#include <anof/domain/transfer/TransferValues.h>
#include <anof/domain/drive/Space.h>
#include <anof/domain/identity/Principal.h>
#include <chrono>
#include <map>
namespace anof {
inline constexpr std::size_t kTreeOperationLimit = 1000;
inline constexpr std::size_t kTreeReadLimit = kTreeOperationLimit + 1;
inline constexpr std::size_t kSearchCandidateLimit = 100000;
struct QueryBudget {
    std::chrono::milliseconds elapsed{100};
    std::uint64_t instructions = 10000000;
    std::size_t candidates = kSearchCandidateLimit;
};
Result<void> validateQueryBudget(const QueryBudget&);
class SearchText {
public:
    static Result<SearchText> parse(std::string_view);
    const std::string& value() const noexcept { return value_; }
private:
    explicit SearchText(std::string value) : value_(std::move(value)) {}
    std::string value_;
};
struct TreeMember { NodeDetails node; std::optional<BlobId> blobId; };
struct TreeSnapshot { NodeId root; std::vector<TreeMember> members; };
struct TreeSummary {
    QuotaDelta total;
    std::map<UserId, QuotaDelta> byCreator;
    std::optional<UserId> soleCreator;
    std::size_t height;
    UtcTime latestEvent;
};
// 输入完整有界树；重复ID、断链、跨空间、环、文件有孩子均为完整性错误。
Result<TreeSummary> summarizeTree(const TreeSnapshot&);
Result<void> requireTreeMutation(const Principal&, const Space&, const TreeSnapshot&);
Result<void> validateTreeDestination(const TreeSnapshot&, const DirectoryPath&);
}  // namespace anof
