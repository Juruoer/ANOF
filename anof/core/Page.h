#pragma once

#include <anof/core/Quantity.h>

#include <optional>
#include <vector>

namespace anof {
using PageSize = Quantity<struct PageSizeTag, 1, 200>;

// 仓储只返回一页自有值；next 是最后一项的位置，不是活跃数据库游标。
template <typename Item, typename Position>
struct Page {
    std::vector<Item> items;
    std::optional<Position> next;
};
}  // namespace anof
