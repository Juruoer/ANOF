#include <anof/application/maintenance/internal/FsckSupport.h>
#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/crypto/SodiumMaintenanceDigest.h>
#include <anof/infrastructure/crypto/SodiumPasswordHasher.h>
#include <anof/infrastructure/crypto/SodiumTransferIdGenerator.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInstanceMaintenance.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataInspection.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataRepair.h>
#include <anof/infrastructure/persistence/sqlite/SqliteMetadataSnapshot.h>
#include <anof/infrastructure/storage/local/LocalMaintenanceFileStore.h>
#include <anof/infrastructure/system/LinuxOfflineInstanceAccess.h>
#include <anof/infrastructure/system/SystemClock.h>
#include <anof/runtime/InstanceIdentity.h>
#include <anof/runtime/InstanceMaintenance.h>
#include <anof/runtime/InstancePasswordPolicy.h>
#include <anof/runtime/OfflineRecovery.h>

#include <stdexcept>
namespace anof::runtime {
namespace {
template <class T>
T create(Result<T> r) {
    if (!r) throw std::runtime_error("maintenance composition failed");
    return std::move(r).value();
}
template <class T>
T require(const MaintenanceContext& c, Result<T> r) {
    if (!r) {
        auto code = r.error().code == ErrorCode::InvalidArgument ? MaintenanceError::InvalidArgument : r.error().code == ErrorCode::CommitOutcomeUnknown ? MaintenanceError::CommitOutcomeUnknown
                                                                                                   : r.error().code == ErrorCode::Retryable              ? MaintenanceError::Busy
                                                                                                                                                         : MaintenanceError::IntegrityError;
        auto e = maintenance::failure(c, code, MaintenanceStage::Recover);
        if (code == MaintenanceError::CommitOutcomeUnknown) e.outcome = MaintenanceOutcome::Unknown;
        throw e;
    }
    return std::move(r).value();
}
void require(const MaintenanceContext& c, Result<void> r) {
    if (!r) {
        auto e = maintenance::failure(c, r.error().code == ErrorCode::CommitOutcomeUnknown ? MaintenanceError::CommitOutcomeUnknown : MaintenanceError::IntegrityError, MaintenanceStage::Recover);
        if (e.code == MaintenanceError::CommitOutcomeUnknown) e.outcome = MaintenanceOutcome::Unknown;
        throw e;
    }
}
}  // namespace
struct InstanceMaintenance::Impl {
    std::unique_ptr<OfflineInstanceAccess> access = create(infrastructure::system::makeLinuxOfflineInstanceAccess());
    std::unique_ptr<MetadataInspection> inspection = create(infrastructure::sqlite::makeSqliteMetadataInspection());
    std::unique_ptr<MetadataRepair> repair = create(infrastructure::sqlite::makeSqliteMetadataRepair());
    std::unique_ptr<MetadataSnapshot> snapshot = create(infrastructure::sqlite::makeSqliteMetadataSnapshot(instancePasswordPolicy().verification));
    std::unique_ptr<MaintenanceFileStore> files = create(infrastructure::storage::makeLocalMaintenanceFileStore());
    std::unique_ptr<InstanceConfigurationLoader> config = create(infrastructure::config::makeTomlInstanceConfigurationLoader());
    std::unique_ptr<MaintenanceRecovery> recovery = makeOfflineMaintenanceRecovery();
    std::unique_ptr<MaintenanceIdGenerator> ids = create(infrastructure::crypto::makeSodiumMaintenanceIdGenerator());
    std::unique_ptr<Clock> clock = create(infrastructure::system::makeSystemClock());
    infrastructure::crypto::SodiumMaintenanceDigest digest;
    FsckService fsck{ *access, *inspection, *files, *repair, *recovery, digest, *ids, *clock };
    BackupService backup{ *access, *inspection, *snapshot, *files, *config, *recovery, *ids, *clock, fsck };
};
InstanceMaintenance::InstanceMaintenance() : impl_(std::make_unique<Impl>()) {}
InstanceMaintenance::~InstanceMaintenance() = default;
FsckService& InstanceMaintenance::fsck() { return impl_->fsck; }
BackupService& InstanceMaintenance::backups() { return impl_->backup; }
MaintenanceResult<InstanceConfiguration> InstanceMaintenance::configuration(const MaintenanceContext& c, const std::filesystem::path& root) {
    try {
        auto lease = maintenance::take(impl_->access->acquireExisting(c, root));
        return require(c, impl_->config->load(*lease));
    } catch (MaintenanceFailure e) {
        return e;
    }
}
MaintenanceResult<void> InstanceMaintenance::resetAdministrator(const MaintenanceContext& c, const std::filesystem::path& root, const PasswordInput& password) {
    bool committing = false;
    try {
        maintenance::check(c);
        auto lease = maintenance::take(impl_->access->acquireExisting(c, root));
        auto verifier = require(c, infrastructure::sqlite::makeSqliteInstanceMaintenance({ instancePasswordPolicy().verification, {} }));
        require(c, verifier->verify(*lease));
        auto resources = require(c, makeInstanceMetadataResources(lease->root() / ".anof/metadata.sqlite3"));
        auto* store = dynamic_cast<DriveMetadataStore*>(resources.metadata.get());
        if (!store) throw maintenance::failure(c, MaintenanceError::InternalError);
        std::optional<PasswordSnapshot> expected;
        std::optional<UserId> administrator;
        {
            auto read = require(c, store->beginDriveRead());
            if (require(c, read->maintenanceState().hasPendingRuns())) throw maintenance::failure(c, MaintenanceError::MaintenancePending);
            std::optional<UserListPosition> after;
            std::size_t count = 0;
            do {
                auto page = require(c, read->users().list(PageSize::from(200).value(), after));
                for (const auto& u : page.items) {
                    if (++count > 100000) throw maintenance::failure(c, MaintenanceError::CheckIncomplete);
                    if (u.role == UserRole::Administrator) {
                        if (administrator || !u.enabled) throw maintenance::failure(c, MaintenanceError::IntegrityError);
                        administrator = u.id;
                    }
                }
                after = page.next;
            } while (after);
            if (!administrator) throw maintenance::failure(c, MaintenanceError::IntegrityError);
            expected = require(c, read->users().findPasswordById(*administrator));
            if (!expected) throw maintenance::failure(c, MaintenanceError::IntegrityError);
        }
        auto hasher = require(c, infrastructure::crypto::makeSodiumPasswordHasher(instancePasswordPolicy()));
        auto hash = require(c, hasher->hash(password));
        maintenance::check(c, MaintenanceStage::Recover);
        auto tx = require(c, store->beginWrite());
        auto now = impl_->clock->now();
        auto user = require(c, tx->users().find(*administrator));
        if (!user || user->role != UserRole::Administrator || !user->enabled) throw maintenance::failure(c, MaintenanceError::IntegrityError);
        require(c, tx->users().replacePassword(*administrator, { expected->passwordVersion, expected->authEpoch, expected->userRevision }, hash, now));
        require(c, tx->sessions().revokeAll(*administrator, now));
        require(c, tx->identityAudit().append({ RequestId::parse(c.operationId.toString()).value(), now, AdministratorPasswordReset{ *administrator } }));
        maintenance::check(c, MaintenanceStage::Recover);
        committing = true;
        require(c, tx->commit());
        return {};
    } catch (MaintenanceFailure e) {
        return e;
    } catch (...) {
        auto e = maintenance::failure(c, MaintenanceError::InternalError, MaintenanceStage::Recover);
        if (committing) e.outcome = MaintenanceOutcome::Unknown;
        return e;
    }
}
}  // namespace anof::runtime
