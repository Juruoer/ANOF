#include <anof/domain/drive/DirectoryPolicy.h>

#include <cstdint>
#include <set>

namespace anof {

Result<void> validateDirectoryName(std::string_view name) {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") return ErrorCode::InvalidName;
    for (std::size_t i = 0; i < name.size();) {
        const auto first = static_cast<unsigned char>(name[i++]);
        std::uint32_t codepoint = first;
        std::size_t extra = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1f;
            extra = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0f;
            extra = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07;
            extra = 3;
            minimum = 0x10000;
        } else if (first >= 0x80) {
            return ErrorCode::InvalidName;
        }
        if (extra > name.size() - i) return ErrorCode::InvalidName;
        for (std::size_t n = 0; n < extra; ++n) {
            const auto next = static_cast<unsigned char>(name[i++]);
            if ((next & 0xc0) != 0x80) return ErrorCode::InvalidName;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return ErrorCode::InvalidName;
        }
        // Unicode Cc 恰为 C0 与 C1；格式字符不属于本规则禁止的控制字符。
        if (codepoint <= 0x1f || (codepoint >= 0x7f && codepoint <= 0x9f) || codepoint == '/' || codepoint == 0x5c) {
            return ErrorCode::InvalidName;
        }
    }
    return {};
}

Result<void> requireCreatePermission(const Principal& principal, const Space& space) {
    switch (space.kind) {
    case SpaceKind::Public:
        if (space.ownerId) return ErrorCode::IntegrityError;
        return {};
    case SpaceKind::Private:
        if (!space.ownerId) return ErrorCode::IntegrityError;
        if (*space.ownerId != principal.userId) return ErrorCode::NotFound;
        return {};
    }
    return ErrorCode::IntegrityError;
}

Result<void> validateDirectoryParent(const Space& space, NodeId parentId, const DirectoryPath& path) {
    if (path.empty() || path.size() > kMaxDirectoryDepth + 1) return ErrorCode::IntegrityError;
    if (path.front().id != space.rootNodeId || path.front().parentId || path.back().id != parentId) {
        return ErrorCode::IntegrityError;
    }
    std::set<NodeId> visited;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i].spaceId != space.id || !visited.insert(path[i].id).second) return ErrorCode::IntegrityError;
        if (i != 0 && (!path[i].parentId || *path[i].parentId != path[i - 1].id)) return ErrorCode::IntegrityError;
    }
    if (path.size() == kMaxDirectoryDepth + 1) return ErrorCode::DepthLimitExceeded;
    return {};
}

Result<std::vector<QuotaScope>> directoryQuotaScopes(const Principal& principal, const Space& space) {
    auto permission = requireCreatePermission(principal, space);
    if (!permission) return permission.error();
    std::vector<QuotaScope> scopes{ SpaceQuota{ space.id } };
    if (space.kind == SpaceKind::Public) scopes.emplace_back(PublicContributionQuota{ space.id, principal.userId });
    return scopes;
}

}  // namespace anof
