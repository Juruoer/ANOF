#include <anof/infrastructure/persistence/sqlite/SqliteInstanceBootstrap.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceFiles.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInitializedState.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataTransaction.h>
#include <anof/infrastructure/persistence/sqlite/SqliteBootstrapInternal.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentitySql.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSchema.h>
#include <new>

namespace anof::infrastructure::sqlite {
namespace {
static_assert(kSqliteSchemaVersion == InitializationIdentity::schemaVersion);
class SqliteInstanceBootstrap final : public InstanceBootstrap {
public:
    SqliteInstanceBootstrap(SqliteBootstrapOptions options, detail::BootstrapHook hook) : options_(options), hook_(hook) {}
    InitResult<InitReceipt> createCurrent(const InstanceLease& lease, const TransferBootstrapSeed& input) override {
        const auto& seed = input.identity;
        auto stage = InitStage::Validate;
        try {
            if (!validateBootstrapSeed(seed)) return InitFailure{ InitError::InvalidInput, stage };
            auto password = EncodedPasswordHash::parse(seed.encodedPassword.view(), options_.passwordHashLimits);
            if (!password) return InitFailure{ InitError::InvalidInput, stage };
            auto files = detail::verifyInitializationFiles(lease, seed.identity, true);
            if (!files) return files.error();
            stage = InitStage::CreateDatabase;
            auto database = detail::openInstanceDatabase(lease, options_.connection, true);
            if (!database) return database.error();
            stage = InitStage::Bootstrap;
            {
                bool active = false;
                SqliteMetadataTransaction tx(*database.value()->connection, active, options_.passwordHashLimits);
                if (auto begun = tx.begin(); !begun) return detail::maintenanceFailure(begun.error().code, stage);
                if (hook_) {
                    auto result = hook_(*database.value()->connection, detail::BootstrapPoint::Begun);
                    if (!result) return detail::maintenanceFailure(result.error().code, stage);
                }
                const NewUser admin{ { seed.adminUserId, seed.adminLogin, seed.adminDisplayName, UserRole::Administrator,
                    true, AuthEpoch{}, Revision{}, seed.createdAt, seed.createdAt }, std::move(password).value(), PasswordVersion{} };
                if (auto inserted = tx.users().insert(admin); !inserted) return detail::maintenanceFailure(inserted.error().code, stage);
                if (auto inserted = tx.settings().insertBootstrap({ seed.identity.instanceId, seed.identity.operationId, seed.createdAt,
                    seed.maxUsers, seed.quotas.privateSpace, seed.quotas.contribution, Revision{} }); !inserted) return detail::maintenanceFailure(inserted.error().code, stage);
                for (const auto& space : { Space{ seed.publicSpaceId, SpaceKind::Public, std::nullopt, seed.publicRootId },
                                          Space{ seed.privateSpaceId, SpaceKind::Private, seed.adminUserId, seed.privateRootId } }) {
                    if (auto inserted = tx.spaces().insert(space); !inserted) return detail::maintenanceFailure(inserted.error().code, stage);
                    if (auto inserted = tx.nodes().insertRoot({ space.rootNodeId, space.id, seed.adminUserId, seed.createdAt }); !inserted) return detail::maintenanceFailure(inserted.error().code, stage);
                    const auto limit = space.kind == SpaceKind::Public ? seed.quotas.publicSpace : seed.quotas.privateSpace;
                    const QuotaSnapshot quota{ { limit, { ByteCount{}, NodeCount::from(1).value() }, { ByteCount{}, NodeCount{} } }, Revision{} };
                    if (auto inserted = tx.quotas().insert(SpaceQuota{ space.id }, quota); !inserted) return detail::maintenanceFailure(inserted.error().code, stage);
                }
                const QuotaSnapshot contribution{ { seed.quotas.contribution, { ByteCount{}, NodeCount::from(1).value() }, { ByteCount{}, NodeCount{} } }, Revision{} };
                if (auto inserted = tx.quotas().insert(PublicContributionQuota{ seed.publicSpaceId, seed.adminUserId }, contribution); !inserted) return detail::maintenanceFailure(inserted.error().code, stage);
                {
                    auto statement = SqliteStatement::prepare(*database.value()->connection,
                        "INSERT INTO storage_accounting VALUES(1,?1,0,0,1)");
                    if (!statement) return detail::maintenanceFailure(statement.error().code, stage);
                    if (auto bound = statement.value()->bindInt64(1, input.contentBudget.value()); !bound)
                        return detail::maintenanceFailure(bound.error().code, stage);
                    if (auto inserted = statement.value()->step(); !inserted)
                        return detail::maintenanceFailure(inserted.error().code, stage);
                }
                if (hook_) {
                    auto result = hook_(*database.value()->connection, detail::BootstrapPoint::BeforeAudit);
                    if (!result) return detail::maintenanceFailure(result.error().code, stage);
                }
                if (auto inserted = tx.identityAudit().append({ seed.requestId, seed.createdAt, InstanceInitialized{ seed.adminUserId, seed.identity.instanceId } }); !inserted) return detail::maintenanceFailure(inserted.error().code, stage);
                if (hook_) {
                    auto result = hook_(*database.value()->connection, detail::BootstrapPoint::BeforeCommit);
                    if (!result) return detail::maintenanceFailure(result.error().code, stage);
                }
                if (auto committed = tx.commit(); !committed) return detail::maintenanceFailure(committed.error().code, stage);
            }
            stage = InitStage::Verify;
            auto checked = detail::verifyInitialized(*database.value()->connection, options_.passwordHashLimits, true);
            if (!checked) return detail::maintenanceFailure(checked.error().code, stage);
            if (auto closed = database.value()->close(lease, true, stage); !closed) return closed.error();
            return checked.value();
        } catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, stage }; }
        catch (const std::filesystem::filesystem_error&) { return InitFailure{ InitError::IoError, stage }; }
    }
    InitResult<InitReceipt> verifyComplete(const InstanceLease& lease, const InitializationIdentity& identity) override {
        constexpr auto stage = InitStage::Verify;
        try {
            if (auto files = detail::verifyInitializationFiles(lease, identity, false); !files) return files.error();
            auto database = detail::openInstanceDatabase(lease, options_.connection, false);
            if (!database) return database.error();
            auto checked = detail::verifyInitialized(*database.value()->connection, options_.passwordHashLimits, true);
            if (!checked) return detail::maintenanceFailure(checked.error().code, stage);
            if (checked.value().identity.instanceId != identity.instanceId || checked.value().identity.operationId != identity.operationId)
                return InitFailure{ InitError::IntegrityError, stage };
            if (auto closed = database.value()->close(lease, true, stage); !closed) return closed.error();
            return checked.value();
        } catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, stage }; }
        catch (const std::filesystem::filesystem_error&) { return InitFailure{ InitError::IoError, stage }; }
    }
private:
    SqliteBootstrapOptions options_;
    detail::BootstrapHook hook_;
};
}
InitResult<std::unique_ptr<InstanceBootstrap>> detail::makeSqliteInstanceBootstrapWith(const SqliteBootstrapOptions& options, BootstrapHook hook) {
    if (!validPasswordLimits(options.passwordHashLimits) || options.connection.busyTimeoutMs < 0 || options.connection.busyTimeoutMs > 5000)
        return InitFailure{ InitError::InvalidInput, InitStage::Validate };
    try { return std::unique_ptr<InstanceBootstrap>(new SqliteInstanceBootstrap(options, hook)); }
    catch (const std::bad_alloc&) { return InitFailure{ InitError::InternalError, InitStage::Validate }; }
}
InitResult<std::unique_ptr<InstanceBootstrap>> makeSqliteInstanceBootstrap(const SqliteBootstrapOptions& options) {
    return detail::makeSqliteInstanceBootstrapWith(options, nullptr);
}
}
