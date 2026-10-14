#include <anof/runtime/InstanceInitialization.h>
#include <anof/runtime/InstancePasswordPolicy.h>
#include <anof/application/maintenance/InitializeInstance.h>
#include <anof/infrastructure/crypto/SodiumIdentityIdGenerator.h>
#include <anof/infrastructure/crypto/SodiumPasswordHasher.h>
#include <anof/infrastructure/crypto/SodiumRequestIdentity.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceBootstrap.h>
#include <anof/infrastructure/system/LinuxInstanceInitializer.h>
#include <anof/infrastructure/system/SystemClock.h>

namespace anof::runtime {
InitResult<InitReceipt> initializeInstance(InitOptions options, PasswordInput password) {
    try {
        auto files = infrastructure::system::makeLinuxInstanceInitializer();
        auto database = infrastructure::sqlite::makeSqliteInstanceBootstrap({ instancePasswordPolicy().verification, {} });
        if (!files) return files.error();
        if (!database) return database.error();
        auto hasher = infrastructure::crypto::makeSodiumPasswordHasher(instancePasswordPolicy());
        auto ids = infrastructure::crypto::makeSodiumIdentityIdGenerator();
        auto requests = infrastructure::crypto::makeSodiumRequestIdentity();
        auto clock = infrastructure::system::makeSystemClock();
        if (!hasher || !ids || !requests || !clock) return InitFailure{ InitError::InternalError, InitStage::Validate };
        InitializeInstance useCase(*files.value(), *database.value(), *hasher.value(), *ids.value(), *clock.value());
        return useCase.execute(std::move(options), std::move(password), requests.value()->forExchange(0, 1));
    } catch (...) { return InitFailure{ InitError::InternalError, InitStage::Validate }; }
}
InitResult<InitReceipt> finishInitialization(const std::filesystem::path& root) {
    try {
        auto files = infrastructure::system::makeLinuxInstanceInitializer();
        auto database = infrastructure::sqlite::makeSqliteInstanceBootstrap({ instancePasswordPolicy().verification, {} });
        if (!files) return files.error();
        if (!database) return database.error();
        return FinishInitialization(*files.value(), *database.value()).execute(root);
    } catch (...) { return InitFailure{ InitError::InternalError, InitStage::Validate }; }
}
}  // namespace anof::runtime
