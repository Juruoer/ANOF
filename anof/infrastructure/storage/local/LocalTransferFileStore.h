#pragma once
#include <anof/application/ports/TransferFileStore.h>

#include <filesystem>
namespace anof::infrastructure::storage {
// 已持有实例 lease 的调用方使用；仅打开已有 uploads/objects/quarantine，不创建实例。
// 调用方保持 lease 至所有 store/handle/hash 销毁。对象打开始终只读。
FileResult<std::unique_ptr<TransferFileStore>> openLocalTransferFileStore(const std::filesystem::path& root);
}  // namespace anof::infrastructure::storage
