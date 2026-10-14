#pragma once
#include <anof/runtime/InstanceInitialization.h>
#include <anof/runtime/InstancePasswordPolicy.h>

namespace anof::testing {
inline InitOptions testInitOptions(const std::filesystem::path& root) {
    const QuotaDelta quota{ByteCount::from(1LL << 40).value(), NodeCount::from(100).value()};
    InstanceConfiguration config;
    config.developmentHttp = true;
    config.port = 0;
    config.metadataQueue = 1;
    config.shutdownGraceSeconds = 0.5;
    return {root, LoginName::parse("administrator").value(), DisplayName::parse("Test administrator").value(),
        {quota, quota, quota}, UserLimit::from(10).value(), config};
}
inline InitResult<InitReceipt> initializeTestInstance(const std::filesystem::path& root) {
    // 仅测试输入；生产入口从终端读取，不提供这个密码或默认账户。
    return runtime::initializeInstance(testInitOptions(root), PasswordInput::parse("fixture password only").value());
}
}  // namespace anof::testing
