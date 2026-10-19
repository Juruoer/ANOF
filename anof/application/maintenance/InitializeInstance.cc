#include <anof/application/maintenance/PrepareInstance.h>
#include <anof/application/maintenance/InitializeInstance.h>

namespace anof {
namespace {
InitFailure unexpected(InitStage stage) {
    // 适配器抛出时不能证明写操作未发生；不自动重试或清理现场。
    if (stage == InitStage::Bootstrap) return { InitError::CommitOutcomeUnknown, stage };
    if (stage == InitStage::PublishFormat || stage == InitStage::ClearPending)
        return { InitError::PublicationOutcomeUnknown, stage };
    return { InitError::InternalError, stage };
}
Result<std::filesystem::path> normalizedRoot(const std::filesystem::path& root) {
    if (!root.is_absolute() || root.native().find('\0') != std::string::npos) return ErrorCode::InvalidArgument;
    std::filesystem::path result = "/";
    for (const auto& part : root.relative_path()) {
        if (part == "..") return ErrorCode::InvalidArgument;
        if (!part.empty() && part != ".") result /= part;
    }
    return result;
}
bool sameIdentity(const InitializationIdentity& a, const InitializationIdentity& b) {
    return a.instanceId == b.instanceId && a.operationId == b.operationId;
}
}
InitResult<InitReceipt> InitializeInstance::execute(TransferInitOptions input, PasswordInput password, RequestId requestId) {
    auto stage = InitStage::Validate;
    auto& options = input.instance;
    try {
        auto root = normalizedRoot(options.absoluteRoot);
        if (!root || !validateInstanceConfiguration(options.configuration) ||
            !validateInitialQuotas(options.quotas) || password.view().empty()) return InitFailure{ InitError::InvalidInput, stage };
        options.absoluteRoot = std::move(root).value();
        stage = InitStage::HashPassword;
        // 哈希和所有 ID 生成都在创建目录与获取数据库事务前完成。
        auto encoded = passwords_.hash(password);
        if (!encoded) return InitFailure{ encoded.error().code == ErrorCode::InvalidArgument ? InitError::InvalidInput : InitError::InternalError, stage };
        stage = InitStage::Validate;
        auto instance = ids_.nextInstance();
        auto operation = ids_.nextOperation();
        auto user = ids_.nextUser();
        auto publicSpace = ids_.nextSpace();
        auto privateSpace = ids_.nextSpace();
        auto publicRoot = ids_.nextNode();
        auto privateRoot = ids_.nextNode();
        if (!instance || !operation || !user || !publicSpace || !privateSpace || !publicRoot || !privateRoot)
            return InitFailure{ InitError::InternalError, stage };
        BootstrapSeed seed{ { instance.value(), operation.value() }, user.value(), std::move(options.adminLogin),
            std::move(options.adminDisplayName), std::move(encoded).value(), publicSpace.value(), privateSpace.value(),
            publicRoot.value(), privateRoot.value(), options.quotas, options.maxUsers, requestId, clock_.now() };
        if (!validateBootstrapSeed(seed)) return InitFailure{ InitError::IntegrityError, stage };
        stage = InitStage::CreateDirectory;
        auto acquired = files_.create(options.absoluteRoot, seed.identity);
        if (!acquired) return acquired.error();
        auto lease = std::move(acquired).value();
        if (!lease || !sameIdentity(lease->identity(), seed.identity) || lease->lease().root() != options.absoluteRoot)
            return InitFailure{ InitError::IntegrityError, stage };
        stage = InitStage::WriteConfiguration;
        if (auto written = lease->writeConfiguration(options.configuration); !written) return written.error();
        stage = InitStage::Bootstrap;
        TransferBootstrapSeed bootstrap{std::move(seed), input.contentBudget};
        const auto& expected = bootstrap.identity;
        auto receipt = database_.createCurrent(lease->lease(), bootstrap);
        if (!receipt) return receipt.error();
        if (!sameIdentity(receipt.value().identity, expected.identity) || receipt.value().adminUserId != expected.adminUserId ||
            receipt.value().publicSpaceId != expected.publicSpaceId || receipt.value().privateSpaceId != expected.privateSpaceId)
            return InitFailure{ InitError::IntegrityError, InitStage::Verify };
        stage = InitStage::PublishFormat;
        if (auto published = lease->publishFormat(); !published) return published.error();
        stage = InitStage::ClearPending;
        if (auto cleared = lease->clearPending(); !cleared) return cleared.error();
        return receipt.value();
    } catch (...) { return unexpected(stage); }
}
InitResult<InitReceipt> FinishInitialization::execute(const std::filesystem::path& root) {
    auto stage = InitStage::Validate;
    try {
        auto normalized = normalizedRoot(root);
        if (!normalized) return InitFailure{ InitError::InvalidInput, stage };
        stage = InitStage::AcquireLock;
        auto acquired = files_.openPending(normalized.value());
        if (!acquired) return acquired.error();
        auto lease = std::move(acquired).value();
        if (!lease || lease->lease().root() != normalized.value()) return InitFailure{ InitError::IntegrityError, stage };
        stage = InitStage::Verify;
        auto receipt = database_.verifyComplete(lease->lease(), lease->identity());
        if (!receipt) return receipt.error();
        if (!sameIdentity(receipt.value().identity, lease->identity())) return InitFailure{ InitError::IntegrityError, stage };
        stage = InitStage::PublishFormat;
        if (auto published = lease->publishFormat(); !published) return published.error();
        stage = InitStage::ClearPending;
        if (auto cleared = lease->clearPending(); !cleared) return cleared.error();
        return receipt.value();
    } catch (...) { return unexpected(stage); }
}
}  // namespace anof
