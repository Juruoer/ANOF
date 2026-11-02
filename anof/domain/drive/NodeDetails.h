#pragma once

#include <anof/core/Page.h>
#include <anof/domain/drive/Node.h>

#include <array>
#include <variant>

namespace anof {
struct FileContentMetadata {
    ByteCount size;
    std::array<std::uint8_t, 32> sha256;
};
struct DirectoryContent {};
// variant 避免 file 没有内容字段/directory 带有 Blob 元数据的矛盾组合。
struct NodeDetails {
    DirectorySnapshot metadata;
    std::variant<DirectoryContent, FileContentMetadata> content;
};
struct ChildPosition {
    std::string name;
    NodeId id;
};
struct RootDirectory {
    NodeId id;
    SpaceId spaceId;
    UserId createdBy;
    UtcTime createdAt;
};
}  // namespace anof
