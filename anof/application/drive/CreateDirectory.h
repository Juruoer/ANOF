#pragma once

#include <anof/application/RequestContext.h>
#include <anof/application/ports/MetadataStore.h>
#include <anof/application/ports/NodeIdGenerator.h>

namespace anof {

struct CreateDirectoryCommand {
    SpaceId spaceId;
    NodeId parentId;
    std::string name;
};

struct CreateDirectoryResult {
    DirectorySnapshot directory;
    Revision parentRevision;
};

// 仅供可信调用方记录/核对失败；候选 ID 不代表提交成功，也不是幂等键。
struct CreateDirectoryAttempt {
    NodeId candidateNodeId;
    RequestId requestId;
};

// 同步阻塞用例，只在元数据 worker 调用；依赖对象必须比本服务存活更久。
class CreateDirectory {
public:
    CreateDirectory(MetadataStore& metadata, Clock& clock, NodeIdGenerator& nodeIds)
        : metadata_(metadata), clock_(clock), nodeIds_(nodeIds) {}

    Result<CreateDirectoryResult> execute(const RequestContext& context, const CreateDirectoryCommand& command,
                                          std::optional<CreateDirectoryAttempt>* attempt = nullptr);

private:
    MetadataStore& metadata_;
    Clock& clock_;
    NodeIdGenerator& nodeIds_;
};

}  // namespace anof
