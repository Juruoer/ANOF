#include <anof/runtime/InstanceIdentity.h>
#include <anof/infrastructure/persistence/sqlite/SqliteAuditRetention.h>
#include <anof/runtime/InstancePasswordPolicy.h>
#include <anof/infrastructure/crypto/SodiumNodeIdGenerator.h>
#include <anof/infrastructure/crypto/SodiumTransferIdGenerator.h>
#include <anof/infrastructure/crypto/SodiumIdentityIdGenerator.h>
#include <anof/infrastructure/crypto/SodiumCredentialIssuer.h>
#include <anof/infrastructure/crypto/SodiumInviteTokenCodec.h>
#include <anof/infrastructure/crypto/SodiumPasswordHasher.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataStore.h>
#include <anof/infrastructure/system/SystemClock.h>

namespace anof::runtime {
Result<MetadataWorkerResources> makeInstanceMetadataResources(const std::filesystem::path& database) {
    auto reopen = [database]() -> Result<std::unique_ptr<TransferMetadataStore>> {
        auto store = infrastructure::sqlite::openSqliteDriveMetadataStore(database, {instancePasswordPolicy().verification, {}});
        if (!store) return store.error();
        return std::unique_ptr<TransferMetadataStore>(std::move(store).value());
    };
    auto store = reopen();
    if (!store) return store.error();
    auto clock = infrastructure::system::makeSystemClock();
    if (!clock) return clock.error();
    auto nodes = infrastructure::crypto::makeSodiumNodeIdGenerator();
    if (!nodes) return nodes.error();
    auto ids = infrastructure::crypto::makeSodiumIdentityIdGenerator();
    if (!ids) return ids.error();
    auto issuer = infrastructure::crypto::makeSodiumCredentialIssuer();
    if (!issuer) return issuer.error();
    auto invites = infrastructure::crypto::makeSodiumInviteTokenCodec();
    if (!invites) return invites.error();
    auto transfers = infrastructure::crypto::makeSodiumTransferIdGenerator();
    if (!transfers) return transfers.error();
    auto driveIds = infrastructure::crypto::makeSodiumDriveIdGenerator();
    if (!driveIds) return driveIds.error();
    auto retention = infrastructure::sqlite::openSqliteAuditRetention(database);
    if (!retention) return retention.error();
    return MetadataWorkerResources{std::move(store).value(), std::move(clock).value(), std::move(nodes).value(),
        std::make_unique<IdentityMetadataResources>(IdentityMetadataResources{
            std::move(ids).value(), std::move(issuer).value(), std::move(invites).value()}),
        std::move(transfers).value(), std::move(reopen), std::move(driveIds).value(), std::move(retention).value()};
}
Result<PasswordWorkerResources> makeInstancePasswordResources() {
    auto hasher = infrastructure::crypto::makeSodiumPasswordHasher(instancePasswordPolicy());
    if (!hasher) return hasher.error();
    // 此值仅用于不存在账户的等成本验证，不创建账户、不生成凭证。每次启动生成新盐。
    auto input = PasswordInput::parse("ANOF dummy verification input");
    if (!input) return input.error();
    auto dummy = hasher.value()->hash(input.value());
    if (!dummy) return dummy.error();
    return PasswordWorkerResources{std::move(hasher).value(), std::move(dummy).value()};
}
} // namespace anof::runtime
