#pragma once
#include <anof/application/ports/TransferMetadataStore.h>
namespace anof {
// 启动期只读对账，不修正计量、不推测丢失对象；调用期间实例尚未接纳请求。
Result<void> auditTransferMetadata(TransferMetadataStore&, UtcTime);
// 有界终态保留清理，物理清理未确认者永不删除。
Result<void> forgetTerminalUploads(TransferMetadataStore&, UtcTime);
}  // namespace anof
