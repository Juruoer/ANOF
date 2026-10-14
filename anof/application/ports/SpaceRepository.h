#pragma once

#include <anof/core/Result.h>
#include <anof/domain/drive/Space.h>
#include <vector>

namespace anof {
class SpaceReader {
public:
    virtual ~SpaceReader() = default;
    virtual Result<std::optional<Space>> find(SpaceId) = 0;
    // 仅查给定用户的私有空间，无匹配返回 nullopt；本方法不执行授权。
    // 应用确认用户存在后，必需私有空间缺失应报告 IntegrityError。
    virtual Result<std::optional<Space>> findPrivate(UserId) = 0;
    // 最多本人私有及公共两项；缺失必需空间为 IntegrityError。
    virtual Result<std::vector<Space>> listAccessible(UserId) = 0;
};
class SpaceRepository : public SpaceReader {
public:
    virtual Result<void> insert(const Space&) = 0;
};
}  // namespace anof
