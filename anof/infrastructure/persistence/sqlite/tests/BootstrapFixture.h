#pragma once
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceBootstrap.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>

#include <boost/test/unit_test.hpp>

namespace anof::infrastructure::sqlite::testing {
inline constexpr PasswordHashLimits bootstrapBudget{ 8192, 1, 1 };
template <typename T>
T bootstrapId(unsigned n) {
    std::string text = "12345678-1234-4234-8234-123456789ab0";
    text.back() = "0123456789abcdef"[n];
    return T::parse(text).value();
}
inline BootstrapSeed bootstrapSeed() {
    const QuotaDelta quota{ ByteCount::from(1024).value(), NodeCount::from(10).value() };
    return { { bootstrapId<InstanceId>(12), bootstrapId<OperationId>(1) }, bootstrapId<UserId>(2), LoginName::parse("admin").value(), DisplayName::parse("Administrator").value(), EncodedPasswordHash::parse("$argon2id$v=19$m=8192,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", bootstrapBudget).value(), bootstrapId<SpaceId>(3), bootstrapId<SpaceId>(4), bootstrapId<NodeId>(5), bootstrapId<NodeId>(6), { quota, quota, quota }, UserLimit::from(10).value(), bootstrapId<RequestId>(7), UtcTime{ std::chrono::microseconds{ 123 } } };
}
inline BootstrapSeed copyBootstrapSeed(const BootstrapSeed& seed) {
    return { seed.identity, seed.adminUserId, seed.adminLogin, seed.adminDisplayName,
             EncodedPasswordHash::parse(seed.encodedPassword.view(), bootstrapBudget).value(),
             seed.publicSpaceId, seed.privateSpaceId, seed.publicRootId, seed.privateRootId,
             seed.quotas, seed.maxUsers, seed.requestId, seed.createdAt };
}
// 既有维护回归只需当前已引导库；真正的文件+数据库初始化由 Bootstrap 专项覆盖。
inline void createBootstrappedTestDatabase(const std::filesystem::path& path) {
    BOOST_REQUIRE(createSqliteDatabase(path));
    auto db = openSqliteDatabase(path);
    BOOST_REQUIRE(db);
    auto seed = bootstrapSeed();
    auto nodes = initialNodeAccounting(seed);
    BOOST_REQUIRE(nodes);
    bool active = false;
    SqliteMetadataTransaction tx(*db.value(), active, bootstrapBudget);
    BOOST_REQUIRE(tx.begin());
    BOOST_REQUIRE(tx.users().insert({ { seed.adminUserId, seed.adminLogin, seed.adminDisplayName, UserRole::Administrator, true,
                                        AuthEpoch{}, Revision{}, seed.createdAt, seed.createdAt },
                                      std::move(seed.encodedPassword),
                                      PasswordVersion{} }));
    BOOST_REQUIRE(tx.settings().insertBootstrap({ seed.identity.instanceId, seed.identity.operationId, seed.createdAt, seed.maxUsers,
                                                  seed.quotas.privateSpace, seed.quotas.contribution, Revision{} }));
    const QuotaSnapshot quota{ { seed.quotas.privateSpace, { ByteCount{}, NodeCount::from(1).value() }, { ByteCount{}, NodeCount{} } }, Revision{} };
    for (auto space : { Space{ seed.publicSpaceId, SpaceKind::Public, std::nullopt, seed.publicRootId }, Space{ seed.privateSpaceId, SpaceKind::Private, seed.adminUserId, seed.privateRootId } }) {
        BOOST_REQUIRE(tx.spaces().insert(space));
        BOOST_REQUIRE(tx.nodes().insertRoot({ space.rootNodeId, space.id, seed.adminUserId, seed.createdAt }));
        BOOST_REQUIRE(tx.quotas().insert(SpaceQuota{ space.id }, quota));
    }
    BOOST_REQUIRE(tx.quotas().insert(PublicContributionQuota{ seed.publicSpaceId, seed.adminUserId }, quota));
    BOOST_REQUIRE(tx.identityAudit().append({ seed.requestId, seed.createdAt, InstanceInitialized{ seed.adminUserId, seed.identity.instanceId } }));
    BOOST_REQUIRE(db.value()->execute("INSERT INTO storage_accounting VALUES(1,1024,0,0,1)"));
    BOOST_REQUIRE(db.value()->execute("INSERT INTO node_accounting VALUES(1," + std::to_string(nodes.value().used.value()) + ",0,1)"));
    BOOST_REQUIRE(tx.commit());
}
}  // namespace anof::infrastructure::sqlite::testing
