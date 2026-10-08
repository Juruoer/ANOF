#pragma once

#include <anof/domain/drive/Node.h>
#include <anof/domain/drive/Quota.h>
#include <anof/domain/drive/Space.h>
#include <anof/domain/identity/Principal.h>

#include <string_view>

namespace anof {

inline constexpr std::size_t kMaxDirectoryDepth = 128;

Result<void> validateDirectoryName(std::string_view name);
Result<void> requireCreatePermission(const Principal& principal, const Space& space);
Result<void> validateDirectoryParent(const Space& space, NodeId parentId, const DirectoryPath& path);
Result<std::vector<QuotaScope>> directoryQuotaScopes(const Principal& principal, const Space& space);

}  // namespace anof
