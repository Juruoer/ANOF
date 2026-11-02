#pragma once

#include <anof/application/ports/MetadataStore.h>

#include <array>
#include <memory>
#include <vector>

namespace anof::testing {

struct UserSeed {
    Principal principal;
    bool enabled = true;
};

struct SessionSeed {
    SessionDigest digest;
    UserId userId;
    AuthEpoch authEpoch;
    UtcTime expiresAt;
    bool revoked = false;
};

struct BlobSeed {
    BlobId id;
    ByteCount size;
    std::array<std::uint8_t, 32> sha256;
};

// 仅测试种子/观察值：blobId 有值即文件；无值即目录。
// trashed 表示该节点自身属于回收站条目，后代还须检查祖先。
struct NodeSeed {
    DirectorySnapshot metadata;
    std::optional<BlobId> blobId;
    bool trashed = false;
};

struct QuotaSeed {
    QuotaScope scope;
    QuotaState state;
};

struct MetadataSeed {
    std::vector<UserSeed> users;
    std::vector<SessionSeed> sessions;
    std::vector<Space> spaces;
    std::vector<BlobSeed> blobs;
    std::vector<NodeSeed> nodes;
    std::vector<QuotaSeed> quotas;
};

// 此接口不是生产 API。后端工厂创建独立的测试目录并负责 RAII 清理。
// backend 必须比连接和事务活得更久；reset 仅在无连接时调用。
// 每次 connect 返回独立 store，连接同一测试数据库，支持真实竞争测试。
// read* 是提交后的观察接口：无活动写事务时调用，不读取未提交变更。
// 迁移、fixture SQL 和物理路径均留在具体后端测试目录。
class TestMetadataBackend {
public:
    virtual ~TestMetadataBackend() = default;
    virtual Result<void> reset(const MetadataSeed& seed) = 0;
    virtual Result<std::unique_ptr<MetadataStore>> connect() = 0;
    virtual Result<std::optional<NodeSeed>> readNode(NodeId id) = 0;
    virtual Result<QuotaState> readQuota(const QuotaScope& scope) = 0;
    virtual Result<std::vector<DirectoryCreatedEvent>> readAudit() = 0;
};

// SQLite 会话在其 tests/ 中实现；共享测试不提供内存替代工厂。
std::unique_ptr<TestMetadataBackend> makeTestMetadataBackend();

}  // namespace anof::testing
