#pragma once
#include <anof/domain/drive/NodeAccounting.h>
namespace anof {
class NodeAccountingReader {
public:
    virtual ~NodeAccountingReader() = default;
    virtual Result<NodeAccounting> get() = 0;
};
class NodeAccountingRepository : public NodeAccountingReader {
public:
    // 与用户/Node/Upload/额度/审计共用事务，语义等同领域纯计算，禁止绝对值覆盖。
    virtual Result<void> chargeUsed(NodeCount) = 0;
    virtual Result<void> releaseUsed(NodeCount) = 0;
    virtual Result<void> reserve(NodeCount) = 0;
    virtual Result<void> commitReservation(NodeCount) = 0;
    virtual Result<void> releaseReservation(NodeCount) = 0;
};
}  // namespace anof
