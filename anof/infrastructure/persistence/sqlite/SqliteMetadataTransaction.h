#pragma once

#include <anof/application/ports/DriveMetadataStore.h>
#include <anof/infrastructure/persistence/sqlite/SqliteAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteAuthorizationReader.h>
#include <anof/infrastructure/persistence/sqlite/SqliteBlobRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteDriveAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteIdentityAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteInviteRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteNodeAccountingRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteNodeRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteQuotaRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSessionRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSettingsRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteSpaceRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteStorageRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTransferAuditRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTrashRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteTreeRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUploadRepository.h>
#include <anof/infrastructure/persistence/sqlite/SqliteUserRepository.h>

namespace anof::infrastructure::sqlite {

// 内部事务实现；生产代码由 MetadataStore 创建，构造本身不启动 SQL 事务。
class SqliteMetadataTransaction final : public DriveTransaction {
public:
    SqliteMetadataTransaction(SqliteConnection& connection, bool& storeActive, PasswordHashLimits limits);
    ~SqliteMetadataTransaction() override;
    Result<void> begin();
    AuthorizationReader& authorization() override { return authorization_; }
    SpaceRepository& spaces() override { return spaces_; }
    FileNodeRepository& nodes() override { return nodes_; }
    DriveQuotaRepository& quotas() override { return quotas_; }
    AuditRepository& directoryAudit() override { return audit_; }
    UserRepository& users() override { return users_; }
    SessionRepository& sessions() override { return sessions_; }
    InviteRepository& invites() override { return invites_; }
    SettingsRepository& settings() override { return settings_; }
    IdentityAuditRepository& identityAudit() override { return identityAudit_; }
    DriveUploadRepository& uploads() override { return uploads_; }
    LifecycleBlobRepository& blobs() override { return blobs_; }
    DriveStorageRepository& storage() override { return storage_; }
    TransferAuditRepository& transferAudit() override { return transferAudit_; }
    TreeRepository& tree() override { return tree_; }
    TrashRepository& trash() override { return trash_; }
    NodeAccountingRepository& nodeAccounting() override { return nodeAccounting_; }
    DriveAuditRepository& driveAudit() override { return driveAudit_; }
    Result<void> commit() override { return context_.commit(); }

private:
    SqliteTransactionContext context_;
    SqliteTreeRepository tree_;
    SqliteTrashRepository trash_;
    SqliteNodeAccountingRepository nodeAccounting_;
    SqliteDriveAuditRepository driveAudit_;
    SqliteAuthorizationReader authorization_;
    SqliteSpaceRepository spaces_;
    SqliteNodeRepository nodes_;
    SqliteQuotaRepository quotas_;
    SqliteAuditRepository audit_;
    SqliteUserRepository users_;
    SqliteSessionRepository sessions_;
    SqliteInviteRepository invites_;
    SqliteSettingsRepository settings_;
    SqliteIdentityAuditRepository identityAudit_;
    SqliteUploadRepository uploads_;
    SqliteBlobRepository blobs_;
    SqliteStorageRepository storage_;
    SqliteTransferAuditRepository transferAudit_;
};

}  // namespace anof::infrastructure::sqlite
