#pragma once
#include <anof/application/maintenance/TransferInitialization.h>

namespace anof::runtime {
// 离线同步装配；不创建 EventLoop、HTTP、Session 或在线密码 worker。
InitResult<InitReceipt> initializeInstance(TransferInitOptions options, PasswordInput password);
InitResult<InitReceipt> finishInitialization(const std::filesystem::path& root);
}  // namespace anof::runtime
