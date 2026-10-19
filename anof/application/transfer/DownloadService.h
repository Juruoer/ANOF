#pragma once
#include <anof/application/ports/TransferMetadataStore.h>
#include <anof/application/transfer/TransferCommands.h>
namespace anof {
struct DownloadGrant {
    UserId user;
    DownloadDescriptor descriptor;
};
// 单一只读快照内授权并复制描述；返回后不保留事务或文件句柄。
Result<DownloadGrant> prepareDownload(TransferMetadataStore&, Clock&, const RequestContext&, NodeId);
}  // namespace anof
