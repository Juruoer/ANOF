#include <anof/application/maintenance/PrepareInstance.h>

#include <cmath>
#include <utility>

namespace anof {

Result<void> validateInstanceConfiguration(const InstanceConfiguration& configuration) {
    if (!isTrustedLoopbackProxy(configuration.listenAddress) ||
        configuration.metadataQueue == 0 || configuration.metadataQueue > 256 ||
        configuration.httpInFlight == 0 || configuration.httpInFlight > 128 ||
        !std::isfinite(configuration.shutdownGraceSeconds) || configuration.shutdownGraceSeconds < 0 ||
        configuration.shutdownGraceSeconds > 30) return ErrorCode::InvalidArgument;
    if (configuration.developmentHttp) {
        if (configuration.webEnabled || !configuration.publicOrigin.empty() || !configuration.trustedProxies.empty())
            return ErrorCode::InvalidArgument;
    } else {
        auto origin = PublicOrigin::parse(configuration.publicOrigin);
        if (!origin || origin.value().value() != configuration.publicOrigin || configuration.trustedProxies.empty() ||
            configuration.trustedProxies.size() > 2) return ErrorCode::InvalidArgument;
        for (const auto& proxy : configuration.trustedProxies)
            if (!isTrustedLoopbackProxy(proxy)) return ErrorCode::InvalidArgument;
        if (configuration.trustedProxies.size() == 2 && configuration.trustedProxies[0] == configuration.trustedProxies[1])
            return ErrorCode::InvalidArgument;
    }
    return {};
}

Result<PreparedInstance> prepareInstance(const std::filesystem::path& root, InstanceLockProvider& locks,
                                         InstanceConfigurationLoader& configuration, InstanceMaintenance& maintenance) {
    try {
        auto locked = locks.acquire(root);
        if (!locked) return locked.error();
        auto lease = std::move(locked).value();
        if (!lease || !lease->root().is_absolute()) return ErrorCode::IntegrityError;
        auto loaded = configuration.load(*lease);
        if (!loaded) return loaded.error();
        auto valid = validateInstanceConfiguration(loaded.value());
        if (!valid) return valid.error();
        auto verified = maintenance.verify(*lease);
        if (!verified) return verified.error();
        return PreparedInstance{ std::move(lease), std::move(loaded).value() };
    } catch (...) {
        // 维护实现的异常不能绕过局部 lease 的 RAII 释放，也不回显文件内容。
        return ErrorCode::InternalError;
    }
}

}  // namespace anof
