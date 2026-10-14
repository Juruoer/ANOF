#pragma once

#include <anof/application/ports/MetadataTransaction.h>
#include <anof/application/ports/MetadataReadSnapshot.h>

#include <memory>

namespace anof {

class MetadataStore {
public:
    virtual ~MetadataStore() = default;
    // 仓储属于同一写事务：授权检查到提交必须受隔离／冲突检测保护。
    // store 必须活得比事务久；不允许同一 store 上嵌套事务（InvalidArgument）。
    // 独立 store 可连接同一后端，以验证真实并发；繁忙时可返回 Retryable。
    virtual Result<std::unique_ptr<MetadataReadSnapshot>> beginRead() = 0;
    virtual Result<std::unique_ptr<MetadataTransaction>> beginWrite() = 0;
};

}  // namespace anof
